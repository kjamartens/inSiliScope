#include "PsfGeneratorBridge.h"
#include "FftRadix2.h"
#include "SplatKernel.h"
#include "ZernikePsf.h"
#include "Parallel.h"
#include "PsfResource.h"
#include "Timing.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
// third_party/jni/{jni.h,win32/jni_md.h}: unmodified headers vendored from
// an OpenJDK/Eclipse Adoptium JDK install (Oracle/OpenJDK, GPLv2 + Classpath
// Exception) -- needed only for correct JNI struct/type layouts so this
// file can compile without requiring a JDK on every build machine. Not
// from, and unrelated to, EPFL's BIG PSFGenerator (see below).
#include <jni.h>
#endif

namespace sim {

int PsfKernelCache::NearestZIndex(double zUm, bool* outClamped) const
{
   if (outClamped)
      *outClamped = false;
   if (nz <= 1 || zStepNm <= 0.0)
      return 0;
   double idxF = zUm * 1000.0 / zStepNm + (nz - 1) / 2.0;
   int idx = static_cast<int>(std::lround(idxF));
   if (idx < 0)
   {
      idx = 0;
      if (outClamped)
         *outClamped = true;
   }
   if (idx >= nz)
   {
      idx = nz - 1;
      if (outClamped)
         *outClamped = true;
   }
   return idx;
}

#ifdef _WIN32

namespace {

const char* ModelName(PsfModelKind m)
{
   switch (m)
   {
      case PsfModelKind::GibsonLanni:
         return "GibsonLanni";
      case PsfModelKind::GibsonLanniZernike:
         return "GibsonLanniZernike";
      case PsfModelKind::RichardsWolf:
      default:
         return "RichardsWolf";
   }
}

///////////////////////////////////////////////////////////////////////////
// Embedded-JVM lifetime (one per process -- JNI_CreateJavaVM only supports
// creating a single JVM per process, so this is created once, lazily, on
// first use, and lives for the DLL's lifetime).
///////////////////////////////////////////////////////////////////////////

typedef jint(JNICALL* CreateJavaVMFunc)(JavaVM**, void**, void*);
typedef jint(JNICALL* GetCreatedJavaVMsFunc)(JavaVM**, jsize, jsize*);

std::mutex g_jvmMutex;
JavaVM* g_jvm = nullptr;
bool g_attachedToForeignJvm = false; // true if g_jvm is a pre-existing JVM we don't own (see EnsureJvmCreated)
jclass g_bridgeClassRef = nullptr;   // global ref, resolved lazily -- see ResolveBridgeClass
std::string g_jvmInitError;          // sticky: if creation failed once, don't keep retrying

// Directory containing this DLL's own file (mmgr_dal_inSiliScope.dll),
// wherever Micro-Manager loaded it from.
std::string OwnModuleDirectory()
{
   HMODULE hModule = NULL;
   if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&OwnModuleDirectory), &hModule))
      return std::string();

   char path[MAX_PATH];
   DWORD len = GetModuleFileNameA(hModule, path, MAX_PATH);
   if (len == 0 || len == MAX_PATH)
      return std::string();

   std::string dllPath(path, len);
   size_t slash = dllPath.find_last_of("\\/");
   if (slash == std::string::npos)
      return std::string();
   return dllPath.substr(0, slash);
}

bool FileExists(const std::string& path)
{
   DWORD attr = GetFileAttributesA(path.c_str());
   return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Locates a JRE/JDK install root (the directory containing
// bin\server\jvm.dll or bin\client\jvm.dll): the request's explicit
// override, else %JAVA_HOME%, else a scan of common Windows install
// locations. Returns "" if none found.
std::string FindJavaHome(const std::string& override)
{
   auto hasJvmDll = [](const std::string& home) {
      return FileExists(home + "\\bin\\server\\jvm.dll") || FileExists(home + "\\bin\\client\\jvm.dll");
   };

   if (!override.empty() && hasJvmDll(override))
      return override;

   char envBuf[MAX_PATH];
   DWORD envLen = GetEnvironmentVariableA("JAVA_HOME", envBuf, MAX_PATH);
   if (envLen > 0 && envLen < MAX_PATH)
   {
      std::string envHome(envBuf, envLen);
      if (hasJvmDll(envHome))
         return envHome;
   }

   // Common install roots for the major Windows JDK distributions.
   const char* roots[] = {
      "C:\\Program Files\\Eclipse Adoptium",
      "C:\\Program Files\\Java",
      "C:\\Program Files\\Zulu",
      "C:\\Program Files\\Microsoft",
      "C:\\Program Files\\Amazon Corretto",
   };
   for (const char* root : roots)
   {
      std::string pattern = std::string(root) + "\\*";
      WIN32_FIND_DATAA fd;
      HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
      if (h == INVALID_HANDLE_VALUE)
         continue;
      do
      {
         if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
         if (std::strcmp(fd.cFileName, ".") == 0 || std::strcmp(fd.cFileName, "..") == 0)
            continue;
         std::string candidate = std::string(root) + "\\" + fd.cFileName;
         if (hasJvmDll(candidate))
         {
            FindClose(h);
            return candidate;
         }
      } while (FindNextFileA(h, &fd));
      FindClose(h);
   }

   return std::string();
}

// Extracts the embedded PSFGenerator+bridge jar (inSiliScope.rc, resource
// IDR_PSF_JAR) to a temp file, once, and returns its path -- JNI classpath
// entries must be real files, not in-memory buffers. Cached for the life
// of the process (the resource never changes without rebuilding the DLL).
std::string ExtractEmbeddedJar(std::string& outError)
{
   static std::string cachedPath;
   static bool attempted = false;
   if (attempted)
      return cachedPath;
   attempted = true;

   HMODULE hModule = NULL;
   GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&ExtractEmbeddedJar), &hModule);

   HRSRC hRes = FindResourceA(hModule, MAKEINTRESOURCEA(IDR_PSF_JAR), RT_RCDATA);
   if (!hRes)
   {
      outError = "Embedded PSFGenerator jar resource not found in this DLL (build problem).";
      return std::string();
   }
   HGLOBAL hData = LoadResource(hModule, hRes);
   if (!hData)
   {
      outError = "Failed to load embedded PSFGenerator jar resource.";
      return std::string();
   }
   DWORD size = SizeofResource(hModule, hRes);
   const void* data = LockResource(hData);
   if (!data || size == 0)
   {
      outError = "Embedded PSFGenerator jar resource is empty.";
      return std::string();
   }

   char tempDir[MAX_PATH];
   GetTempPathA(MAX_PATH, tempDir);
   std::string tempPath = std::string(tempDir) + "inSiliScope_PsfGenerator.jar";

   HANDLE hFile = CreateFileA(tempPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
   if (hFile == INVALID_HANDLE_VALUE)
   {
      outError = "Failed to create temp file for embedded PSFGenerator jar: " + tempPath;
      return std::string();
   }
   DWORD written = 0;
   BOOL ok = WriteFile(hFile, data, size, &written, NULL);
   CloseHandle(hFile);
   if (!ok || written != size)
   {
      outError = "Failed to write embedded PSFGenerator jar to temp file: " + tempPath;
      return std::string();
   }

   cachedPath = tempPath;
   return cachedPath;
}

// Creates the process-wide embedded JVM if not already created. Returns
// false (outError set) if no JVM could be created -- callers should not
// retry (g_jvmInitError is sticky) since a fresh attempt won't succeed any
// more than the first one did.
bool EnsureJvmCreated(const std::string& javaHomeOverride, std::string& outError)
{
   std::lock_guard<std::mutex> lock(g_jvmMutex);
   if (g_jvm != nullptr)
      return true;
   if (!g_jvmInitError.empty())
   {
      outError = g_jvmInitError;
      return false;
   }

   // Classic Micro-Manager (MMStudio) is itself a Java application: its
   // own launcher JVM loads MMStudio.jar, which loads MMCoreJ_wrap (a JNI
   // native library) into that SAME process, which in turn loads MMCore
   // and every device adapter DLL -- including this one -- natively, all
   // within that one process/address space. So by the time this code
   // runs, a JVM is very likely ALREADY active in-process. The JNI
   // Invocation API does not support creating a second JVM in a process
   // that already has one -- HotSpot in particular does not fail
   // gracefully when you try (no catchable exception, no clean JNI_ERR):
   // it can crash the whole host process outright, which is exactly what
   // was observed (Micro-Manager's corelog just stops, no exception
   // logged, when selecting a diffraction PsfModel). If jvm.dll is already
   // loaded into this process, reuse whatever JVM instance it already
   // created via JNI_GetCreatedJavaVMs instead of calling
   // JNI_CreateJavaVM ourselves.
   HMODULE hExistingJvmDll = GetModuleHandleA("jvm.dll");
   if (hExistingJvmDll)
   {
      GetCreatedJavaVMsFunc getCreatedVMs =
         reinterpret_cast<GetCreatedJavaVMsFunc>(GetProcAddress(hExistingJvmDll, "JNI_GetCreatedJavaVMs"));
      JavaVM* existingVms[1] = {nullptr};
      jsize numVMs = 0;
      if (getCreatedVMs && getCreatedVMs(existingVms, 1, &numVMs) == JNI_OK && numVMs > 0 &&
          existingVms[0] != nullptr)
      {
         g_jvm = existingVms[0];
         g_attachedToForeignJvm = true;
         return true;
      }
      // jvm.dll is loaded but reports no created VM yet -- fall through
      // and try creating our own (unusual, but not impossible).
   }

   std::string javaHome = FindJavaHome(javaHomeOverride);
   if (javaHome.empty())
   {
      g_jvmInitError = "No Java runtime found (set PsfGeneratorJavaHome, or install a JDK/JRE and/or set "
                        "the JAVA_HOME environment variable).";
      outError = g_jvmInitError;
      return false;
   }

   std::string jvmDllPath = javaHome + "\\bin\\server\\jvm.dll";
   if (!FileExists(jvmDllPath))
      jvmDllPath = javaHome + "\\bin\\client\\jvm.dll";

   HMODULE hJvmDll = LoadLibraryA(jvmDllPath.c_str());
   if (!hJvmDll)
   {
      g_jvmInitError = "Failed to load jvm.dll from: " + jvmDllPath;
      outError = g_jvmInitError;
      return false;
   }

   CreateJavaVMFunc createJavaVM =
      reinterpret_cast<CreateJavaVMFunc>(GetProcAddress(hJvmDll, "JNI_CreateJavaVM"));
   if (!createJavaVM)
   {
      g_jvmInitError = "jvm.dll at " + jvmDllPath + " has no JNI_CreateJavaVM export.";
      outError = g_jvmInitError;
      return false;
   }

   std::string jarPath = ExtractEmbeddedJar(outError);
   if (jarPath.empty())
   {
      g_jvmInitError = outError;
      return false;
   }

   std::string classpathOpt = "-Djava.class.path=" + jarPath;
   std::vector<char> classpathBuf(classpathOpt.begin(), classpathOpt.end());
   classpathBuf.push_back('\0');

   JavaVMOption options[1];
   options[0].optionString = classpathBuf.data();

   JavaVMInitArgs vmArgs{};
   vmArgs.version = JNI_VERSION_1_8;
   vmArgs.nOptions = 1;
   vmArgs.options = options;
   vmArgs.ignoreUnrecognized = JNI_FALSE;

   JNIEnv* env = nullptr;
   jint rc = createJavaVM(&g_jvm, reinterpret_cast<void**>(&env), &vmArgs);
   if (rc != JNI_OK || g_jvm == nullptr)
   {
      g_jvm = nullptr;
      g_jvmInitError = "JNI_CreateJavaVM failed (rc=" + std::to_string(rc) + ") using " + jvmDllPath;
      outError = g_jvmInitError;
      return false;
   }

   return true;
}

// Reads the message of a pending Java exception (if any) into a C++
// string, and clears it. Returns "" if there is no pending exception.
std::string DescribeAndClearException(JNIEnv* env)
{
   if (!env->ExceptionCheck())
      return std::string();

   jthrowable ex = env->ExceptionOccurred();
   env->ExceptionClear();
   if (!ex)
      return "Unknown Java exception";

   jclass throwableClass = env->GetObjectClass(ex);
   jmethodID getMessage = env->GetMethodID(throwableClass, "getMessage", "()Ljava/lang/String;");
   std::string result = "Java exception (no message available)";
   if (getMessage)
   {
      jstring msg = static_cast<jstring>(env->CallObjectMethod(ex, getMessage));
      if (msg)
      {
         const char* utf = env->GetStringUTFChars(msg, nullptr);
         if (utf)
         {
            result = utf;
            env->ReleaseStringUTFChars(msg, utf);
         }
         env->DeleteLocalRef(msg);
      }
   }
   env->DeleteLocalRef(throwableClass);
   env->DeleteLocalRef(ex);
   return result;
}

// Resolves psfbridge.PsfBridge (this project's own class, embedded together
// with BIG PSFGenerator's classes in the jar baked into this DLL -- see
// inSiliScope.rc / ExtractEmbeddedJar above) and caches it as a global ref.
// Deliberately does NOT rely on java.class.path / the JVM's default
// (system/application) classloader: when g_attachedToForeignJvm is true
// (the common case under classic Micro-Manager -- see EnsureJvmCreated),
// that classloader belongs to the host application (MMStudio) and knows
// nothing about our embedded jar, and mutating a foreign process's
// classpath after the fact isn't possible via the JNI Invocation API.
// Instead this explicitly builds a java.net.URLClassLoader pointing at our
// extracted-to-temp jar and loads the class through that -- correct and
// identical whether g_jvm was created by us or is a pre-existing one.
jclass ResolveBridgeClass(JNIEnv* env, std::string& outError)
{
   {
      std::lock_guard<std::mutex> lock(g_jvmMutex);
      if (g_bridgeClassRef)
         return g_bridgeClassRef;
   }

   std::string jarPath = ExtractEmbeddedJar(outError);
   if (jarPath.empty())
      return nullptr;

   jclass fileClass = env->FindClass("java/io/File");
   jclass uriClass = env->FindClass("java/net/URI");
   jclass urlClass = env->FindClass("java/net/URL");
   jclass classLoaderClass = env->FindClass("java/net/URLClassLoader");
   jclass classClass = env->FindClass("java/lang/Class");
   if (!fileClass || !uriClass || !urlClass || !classLoaderClass || !classClass)
   {
      outError = "Core JDK classes (File/URI/URL/URLClassLoader/Class) not found: " + DescribeAndClearException(env);
      return nullptr;
   }

   jmethodID fileCtor = env->GetMethodID(fileClass, "<init>", "(Ljava/lang/String;)V");
   jmethodID toURI = env->GetMethodID(fileClass, "toURI", "()Ljava/net/URI;");
   jmethodID toURL = env->GetMethodID(uriClass, "toURL", "()Ljava/net/URL;");
   jmethodID classLoaderCtor = env->GetMethodID(classLoaderClass, "<init>", "([Ljava/net/URL;)V");
   jmethodID forNameMethod = env->GetStaticMethodID(
      classClass, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
   if (!fileCtor || !toURI || !toURL || !classLoaderCtor || !forNameMethod)
   {
      outError = "Core JDK method lookup failed: " + DescribeAndClearException(env);
      return nullptr;
   }

   jstring jarPathStr = env->NewStringUTF(jarPath.c_str());
   jobject fileObj = env->NewObject(fileClass, fileCtor, jarPathStr);
   jobject uriObj = fileObj ? env->CallObjectMethod(fileObj, toURI) : nullptr;
   jobject urlObj = uriObj ? env->CallObjectMethod(uriObj, toURL) : nullptr;
   std::string exMsg = DescribeAndClearException(env);
   if (!exMsg.empty() || !urlObj)
   {
      outError = "Failed to build file:// URL for embedded jar (" + jarPath + "): " + exMsg;
      return nullptr;
   }

   jobjectArray urlArray = env->NewObjectArray(1, urlClass, urlObj);
   jobject classLoaderObj = env->NewObject(classLoaderClass, classLoaderCtor, urlArray);
   exMsg = DescribeAndClearException(env);
   if (!exMsg.empty() || !classLoaderObj)
   {
      outError = "Failed to construct URLClassLoader for embedded jar: " + exMsg;
      return nullptr;
   }

   jstring className = env->NewStringUTF("psfbridge.PsfBridge");
   jobject bridgeClassObj = env->CallStaticObjectMethod(classClass, forNameMethod, className, JNI_TRUE, classLoaderObj);
   exMsg = DescribeAndClearException(env);
   if (!exMsg.empty() || !bridgeClassObj)
   {
      outError = "Class.forName(\"psfbridge.PsfBridge\") failed: " + exMsg;
      return nullptr;
   }

   std::lock_guard<std::mutex> lock(g_jvmMutex);
   if (!g_bridgeClassRef)
      g_bridgeClassRef = static_cast<jclass>(env->NewGlobalRef(bridgeClassObj));
   return g_bridgeClassRef;
}

} // namespace

namespace {

// RichardsWolf / GibsonLanni: PSFGenerator in the embedded JVM.
bool ComputePsfKernelCacheJvm(const PsfGeneratorRequest& req, PsfKernelCache& outCache, std::string& outError,
                              const std::function<void(const std::string&)>& logCallback)
{
   outCache = PsfKernelCache();
   outError.clear();

   if (!EnsureJvmCreated(req.javaHome, outError))
      return false;

   JNIEnv* env = nullptr;
   // Each call may arrive from a different long-lived MM thread (the Live
   // producer thread, or a stack-generation worker thread) -- JNIEnv is
   // thread-specific, so attach/detach around every call rather than
   // caching one.
   jint attachRc = g_jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr);
   if (attachRc != JNI_OK || env == nullptr)
   {
      outError = "AttachCurrentThread failed (rc=" + std::to_string(attachRc) + ")";
      return false;
   }

   bool ok = false;
   do
   {
      // psfbridge.PsfBridge (this project's own class, Simulation/psfbridge-java/
      // psfbridge/PsfBridge.java) is the sole entry point called here; it in
      // turn instantiates and drives EPFL Biomedical Imaging Group's BIG
      // PSFGenerator classes (psf.richardswolf.RichardsWolfPSF /
      // psf.gibsonlanni.GibsonLanniPSF, GPL-3.0) -- both are compiled
      // together into the single jar embedded in this DLL (inSiliScope.rc,
      // IDR_PSF_JAR). See PsfBridge.java's header comment for the full
      // attribution and what PSFGenerator code path this exercises.
      // ResolveBridgeClass loads this via an explicit URLClassLoader
      // pointing at the embedded jar, rather than plain FindClass -- when
      // g_attachedToForeignJvm is true (classic Micro-Manager's own JVM,
      // the common case -- see EnsureJvmCreated), the default/system
      // classloader is the HOST application's and has no knowledge of our
      // embedded jar. The returned jclass is a cached global ref (shared,
      // reused across calls) -- do not DeleteLocalRef it.
      jclass cls = ResolveBridgeClass(env, outError);
      if (!cls)
         break;
      jmethodID method =
         env->GetStaticMethodID(cls, "computePlanes",
                                 "(Ljava/lang/String;DDDDDDDDIIILjava/lang/String;Ljava/lang/String;ID)[F");
      if (!method)
      {
         outError = "psfbridge.PsfBridge.computePlanes not found: " + DescribeAndClearException(env);
         break;
      }

      int oversampling = std::max(1, req.oversampling);
      int camHalf = std::max(1, req.kernelHalfWidthPx);
      int halfOv = camHalf * oversampling;
      int size = 2 * halfOv + 1;
      // PSFGenerator's own PSF.checkSize() requires nz >= 3; keep it odd
      // so a true center (in-focus) plane exists.
      int nzWanted = std::max(3, req.nz);
      int nz = (nzWanted % 2 == 1) ? nzWanted : nzWanted + 1;
      double resLateralNm = req.pixelSizeNm / oversampling;

      jstring modelStr = env->NewStringUTF(ModelName(req.model));
      jstring zernikeStr = env->NewStringUTF(req.zernikeCoefficients.c_str());
      jstring maskTypeStr =
         env->NewStringUTF(req.maskType == PsfMaskType::DoubleHelix ? "doubleHelix" : "none");

      if (logCallback)
      {
         std::ostringstream startMsg;
         startMsg << "PSFGenerator: computing " << ModelName(req.model) << " PSF kernel (" << size << "x" << size
                   << " px, " << nz << (nz == 1 ? " Z plane" : " Z planes") << ")...";
         logCallback(startMsg.str());

      }

      // The JNI call below blocks synchronously for the entire computation
      // (see PsfBridge.java's class Javadoc on why -- no incremental
      // progress crosses the JNI boundary). This heartbeat thread does no
      // JNI work itself (just sleeps and calls back into logCallback, which
      // is plain C++/MMDevice logging) -- purely so a slow model (e.g.
      // GibsonLanniZernike at a large window/many planes) doesn't look hung
      // in corelog.
      auto startTime = std::chrono::steady_clock::now();
      std::atomic<bool> stopHeartbeat{false};
      std::thread heartbeat;
      if (logCallback)
      {
         heartbeat = std::thread([&]() {
            while (!stopHeartbeat.load(std::memory_order_relaxed))
            {
               for (int i = 0; i < 20 && !stopHeartbeat.load(std::memory_order_relaxed); ++i)
                  std::this_thread::sleep_for(std::chrono::milliseconds(100));
               if (stopHeartbeat.load(std::memory_order_relaxed))
                  break;
               double elapsedS = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
               std::ostringstream msg;
               msg << "PSFGenerator: still computing... " << std::fixed << std::setprecision(1) << elapsedS
                   << "s elapsed";
               logCallback(msg.str());
            }
         });
      }

      jobject result = env->CallStaticObjectMethod(cls, method, modelStr, static_cast<jdouble>(req.na),
                                                     static_cast<jdouble>(req.wavelengthNm),
                                                     static_cast<jdouble>(req.immersionIndex),
                                                     static_cast<jdouble>(req.sampleIndex),
                                                     static_cast<jdouble>(req.workingDistanceUm),
                                                     static_cast<jdouble>(req.sampleDepthNm),
                                                     static_cast<jdouble>(resLateralNm),
                                                     static_cast<jdouble>(req.zStepNm), static_cast<jint>(size),
                                                     static_cast<jint>(size), static_cast<jint>(nz), zernikeStr,
                                                     maskTypeStr, static_cast<jint>(req.maskModes),
                                                     static_cast<jdouble>(req.maskWaist));

      stopHeartbeat.store(true, std::memory_order_relaxed);
      if (heartbeat.joinable())
         heartbeat.join();

      if (logCallback)
      {
         double elapsedS = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
         std::ostringstream doneMsg;
         doneMsg << "PSFGenerator: kernel computation finished in " << std::fixed << std::setprecision(2) << elapsedS
                 << "s";
         logCallback(doneMsg.str());
      }

      env->DeleteLocalRef(modelStr);
      env->DeleteLocalRef(zernikeStr);
      env->DeleteLocalRef(maskTypeStr);
      // Note: cls is the cached global ref from ResolveBridgeClass -- not
      // deleted here (see the comment where it was obtained above).

      std::string exMsg = DescribeAndClearException(env);
      if (!exMsg.empty())
      {
         outError = "PSFGenerator computation failed: " + exMsg;
         break;
      }
      if (!result)
      {
         outError = "psfbridge.PsfBridge.computePlanes returned null.";
         break;
      }

      jfloatArray floatArray = static_cast<jfloatArray>(result);
      jsize len = env->GetArrayLength(floatArray);
      size_t planeFloats = static_cast<size_t>(size) * static_cast<size_t>(size);
      size_t expectedLen = planeFloats * static_cast<size_t>(nz);
      if (static_cast<size_t>(len) != expectedLen)
      {
         outError = "psfbridge.PsfBridge.computePlanes returned " + std::to_string(len) + " floats, expected " +
                     std::to_string(expectedLen) + ".";
         env->DeleteLocalRef(result);
         break;
      }

      std::vector<float> flat(expectedLen);
      env->GetFloatArrayRegion(floatArray, 0, len, flat.data());
      env->DeleteLocalRef(result);

      outCache.oversampling = oversampling;
      outCache.halfWidthOversampled = halfOv;
      outCache.sizeOversampled = size;
      outCache.nz = nz;
      outCache.zStepNm = req.zStepNm;
      outCache.interpMode = req.interpMode;
      outCache.planes.assign(static_cast<size_t>(nz), std::vector<float>(planeFloats));
      outCache.blockSums.assign(static_cast<size_t>(nz), std::vector<float>());
      outCache.blockSumWidth = size + oversampling - 1;
      for (int z = 0; z < nz; ++z)
      {
         std::vector<float>& plane = outCache.planes[static_cast<size_t>(z)];
         std::memcpy(plane.data(), flat.data() + static_cast<size_t>(z) * planeFloats, planeFloats * sizeof(float));
         // Photon-normalize (sum 1): each entry becomes a probability mass,
         // so a splat of N photons deposits N (minus what leaves the image).
         double sum = 0.0;
         for (float v : plane)
            sum += v;
         if (sum > 0.0)
            for (float& v : plane)
               v = static_cast<float>(v / sum);
         outCache.blockSums[static_cast<size_t>(z)] = BuildBlockSums(plane.data(), size, oversampling);
      }
      BuildPolyphaseSums(outCache);
      outCache.valid = true;
      ok = true;
   } while (false);

   g_jvm->DetachCurrentThread();
   return ok;
}

} // namespace

#endif // _WIN32

namespace {

// Everything the computed planes depend on: the request minus interpMode
// (copied into the cache, not used to compute it) and javaHome (only picks
// the JVM, which is created once per process).
bool SameKernel(const PsfGeneratorRequest& a, const PsfGeneratorRequest& b)
{
   return a.model == b.model && a.wavelengthNm == b.wavelengthNm && a.na == b.na &&
          a.immersionIndex == b.immersionIndex && a.sampleIndex == b.sampleIndex &&
          a.workingDistanceUm == b.workingDistanceUm && a.sampleDepthNm == b.sampleDepthNm &&
          a.pixelSizeNm == b.pixelSizeNm && a.zernikeCoefficients == b.zernikeCoefficients &&
          a.maskType == b.maskType && a.maskModes == b.maskModes && a.maskWaist == b.maskWaist &&
          a.oversampling == b.oversampling && a.kernelHalfWidthPx == b.kernelHalfWidthPx && a.nz == b.nz &&
          a.zStepNm == b.zStepNm;
}

// The last few kernels computed in this process: a live-mode config change
// or a new stack that leaves the PSF parameters alone (exposure, gain,
// seed, ...) gets the kernel it would recompute.
struct KernelMemo
{
   std::mutex mutex;
   std::vector<std::pair<PsfGeneratorRequest, std::shared_ptr<const PsfKernelCache>>> entries; // most recent first
};

KernelMemo& Memo()
{
   static KernelMemo* memo = new KernelMemo(); // never destroyed (DLL unload order)
   return *memo;
}

constexpr size_t kKernelMemoEntries = 2;

// GibsonLanniZernike: the C++ port (ZernikePsf.cpp; the Java class stays as
// its reference). RichardsWolf / GibsonLanni: the JVM, Windows only.
bool ComputePsfKernelCacheUncached(const PsfGeneratorRequest& req, PsfKernelCache& outCache, std::string& outError,
                                   const std::function<void(const std::string&)>& logCallback)
{
   if (req.model == PsfModelKind::GibsonLanniZernike)
   {
      const auto startTime = std::chrono::steady_clock::now();
      if (logCallback)
      {
         std::ostringstream msg;
         msg << "PSF: computing the GibsonLanniZernike kernel (C++, " << (2 * std::max(1, req.kernelHalfWidthPx) * std::max(1, req.oversampling) + 1)
             << " px square, " << std::max(3, req.nz) << " Z planes)...";
         logCallback(msg.str());
      }
      if (!BuildZernikePsfKernelCache(req, outCache, outError))
         return false;
      if (logCallback)
      {
         const double elapsedS = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
         std::ostringstream msg;
         msg << "PSF: GibsonLanniZernike kernel finished in " << std::fixed << std::setprecision(2) << elapsedS << "s";
         logCallback(msg.str());
      }
      return true;
   }
#ifdef _WIN32
   return ComputePsfKernelCacheJvm(req, outCache, outError, logCallback);
#else
   (void)logCallback;
   outCache = PsfKernelCache();
   outError = "The RichardsWolf/GibsonLanni models need the embedded PSFGenerator JVM bridge, only implemented for Windows.";
   return false;
#endif
}

} // namespace

bool ComputePsfKernelCache(const PsfGeneratorRequest& req, PsfKernelCache& outCache, std::string& outError,
                            const std::function<void(const std::string&)>& logCallback)
{
   outError.clear();
   const auto tStart = TimingClock::now();
   {
      KernelMemo& m = Memo();
      std::lock_guard<std::mutex> g(m.mutex);
      for (size_t i = 0; i < m.entries.size(); ++i)
      {
         if (!SameKernel(m.entries[i].first, req))
            continue;
         outCache = *m.entries[i].second;
         outCache.interpMode = req.interpMode;
         std::rotate(m.entries.begin(), m.entries.begin() + i, m.entries.begin() + i + 1);
         if (logCallback)
            logCallback("PSF: kernel unchanged, reusing the one already computed.");
         TimingLog("psf.memo-hit", TimingSince(tStart));
         return true;
      }
   }
   if (!ComputePsfKernelCacheUncached(req, outCache, outError, logCallback))
      return false;
   TimingLog("psf.compute", TimingSince(tStart));
   const auto tStore = TimingClock::now();
   KernelMemo& m = Memo();
   std::lock_guard<std::mutex> g(m.mutex);
   m.entries.insert(m.entries.begin(), std::make_pair(req, std::make_shared<const PsfKernelCache>(outCache)));
   if (m.entries.size() > kKernelMemoEntries)
      m.entries.resize(kKernelMemoEntries);
   TimingLog("psf.memo-store", TimingSince(tStore));
   return true;
}



int PsfKernelHalfWidthPx(double halfWidthNm, double pixelSizeNm, double wavelengthNm, double na)
{
   int requested = static_cast<int>(std::lround(halfWidthNm / pixelSizeNm));
   requested = std::max(requested, 1);
   const double naSafe = na > 0.0 ? na : 0.01;
   const double rayleighRadiusNm = 0.61 * wavelengthNm / naSafe;
   int minHalf = static_cast<int>(std::ceil(3.0 * rayleighRadiusNm / pixelSizeNm));
   minHalf = std::min(std::max(minHalf, 2), 48);
   return std::max(requested, minHalf);
}

std::vector<float> BuildBlockSums(const float* kernel, int n, int os)
{
   // Port of webSMLM's buildSummedKernel(): horizontal block sums per kernel
   // row, then vertical sums of those. B spans a in [-(os-1), n-1] (every
   // block that overlaps the kernel at all), stored with offset off = os-1.
   const int off = os - 1, bw = n + off;
   std::vector<double> hsum(static_cast<size_t>(bw) * n);
   for (int y = 0; y < n; ++y)
      for (int bi = 0; bi < bw; ++bi)
      {
         const int b = bi - off, x0 = std::max(0, b), x1 = std::min(n - 1, b + off);
         double acc = 0.0;
         for (int x = x0; x <= x1; ++x)
            acc += kernel[static_cast<size_t>(y) * n + x];
         hsum[static_cast<size_t>(y) * bw + bi] = acc;
      }
   std::vector<float> out(static_cast<size_t>(bw) * bw);
   for (int ai = 0; ai < bw; ++ai)
   {
      const int a = ai - off, y0 = std::max(0, a), y1 = std::min(n - 1, a + off);
      for (int bi = 0; bi < bw; ++bi)
      {
         double acc = 0.0;
         for (int y = y0; y <= y1; ++y)
            acc += hsum[static_cast<size_t>(y) * bw + bi];
         out[static_cast<size_t>(ai) * bw + bi] = static_cast<float>(acc);
      }
   }
   return out;
}

std::vector<float> BuildPolyphaseSums(const float* blockSums, int bw, int os)
{
   std::vector<float> out;
   if (bw <= 0 || os <= 0 || bw % os != 0)
      return out;
   const int qw = bw / os;
   out.resize(static_cast<size_t>(bw) * bw);
   for (int r = 0; r < bw; ++r)
   {
      const float* src = blockSums + static_cast<size_t>(r) * bw;
      float* dst = out.data() + static_cast<size_t>(r) * os * qw;
      for (int c = 0; c < bw; ++c)
         dst[static_cast<size_t>(c % os) * qw + c / os] = src[c];
   }
   return out;
}

void BuildPolyphaseSums(PsfKernelCache& cache)
{
   cache.polySums.assign(cache.blockSums.size(), std::vector<float>());
   ParallelFor(static_cast<unsigned>(cache.blockSums.size()), [&](unsigned z) {
      cache.polySums[z] = BuildPolyphaseSums(cache.blockSums[z].data(), cache.blockSumWidth, cache.oversampling);
   });
}

namespace {

// JS Math.round (ties toward +infinity), which webSMLM's splat uses.
double RoundHalfUp(double v)
{
   return std::floor(v + 0.5);
}

void CatmullRomWeights(double f, double* w)
{
   const double f2 = f * f, f3 = f2 * f;
   w[0] = -0.5 * f3 + f2 - 0.5 * f;
   w[1] = 1.5 * f3 - 2.5 * f2 + 1.0;
   w[2] = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
   w[3] = 0.5 * f3 - 0.5 * f2;
}

// Shifts one zero-padded complex line (length N, first n entries live) by
// `shift` samples via the Fourier shift theorem: g(i) = f(i+shift), wrapped
// (k >= N/2 -> k-N) frequencies. phase[k] = exp(+2*pi*i*kk*shift/N) is
// precomputed by the caller (one table per axis), and so are the forward and
// inverse twiddles.
void FourierShiftLine(std::vector<double>& re, std::vector<double>& im, int N, const std::vector<double>& pc,
                      const std::vector<double>& ps, const FftTwiddles& fwd, const FftTwiddles& inv)
{
   Fft1d(re.data(), im.data(), fwd);
   for (int k = 0; k < N; ++k)
   {
      const double r = re[k], i = im[k];
      re[k] = r * pc[k] - i * ps[k];
      im[k] = r * ps[k] + i * pc[k];
   }
   Fft1d(re.data(), im.data(), inv);
   const double norm = 1.0 / N;
   for (int k = 0; k < N; ++k)
   {
      re[k] *= norm;
      im[k] *= norm;
   }
}

// Port of webSMLM's fftShiftKernelTile(): shifts a real n x n kernel tile by
// a continuous (shiftX, shiftY), in kernel-grid units, via the Fourier
// shift theorem on a zero-padded (>= 2x) buffer -- g(i) = f(i+shift),
// using wrapped (k >= N/2 -> k-N) frequencies. webSMLM does one N x N 2D
// FFT pair plus N^2 phase factors; the shift phase exp(i(kx sx + ky sy)) is
// separable, so this does the IDENTICAL linear operation as 1D shifts along
// every row and then every column (the intermediate kept complex, outputs
// outside the n x n tile never needed) -- the same result, ~N/n times less
// work and 2N instead of N^2 trig pairs. Still one transform per emitter,
// so Fft remains the slowest placement mode.
std::vector<float> FftShiftKernelTile(const float* kernel, int n, double shiftX, double shiftY, bool parallel)
{
   int N = 1;
   while (N < 2 * n)
      N <<= 1;
   const int half = N / 2;
   auto phaseTable = [&](double shift, std::vector<double>& pc, std::vector<double>& ps) {
      pc.resize(N);
      ps.resize(N);
      for (int k = 0; k < N; ++k)
      {
         const int kk = k < half ? k : k - N;
         const double ang = 2.0 * 3.14159265358979323846 * kk * shift / N;
         pc[k] = std::cos(ang);
         ps[k] = std::sin(ang);
      }
   };
   std::vector<double> xc, xs, yc, ys;
   phaseTable(shiftX, xc, xs);
   phaseTable(shiftY, yc, ys);
   static const FftTwiddles* cached[2] = {nullptr, nullptr};
   static std::mutex cachedMutex;
   const FftTwiddles *fwd, *inv;
   {
      // One pair per process for the current N (N depends only on the
      // kernel size); rebuilt, never freed while in use, when N changes.
      std::lock_guard<std::mutex> g(cachedMutex);
      static std::vector<std::unique_ptr<FftTwiddles>> keep;
      if (!cached[0] || cached[0]->n != N)
      {
         keep.emplace_back(new FftTwiddles(MakeTwiddles(N, -1)));
         cached[0] = keep.back().get();
         keep.emplace_back(new FftTwiddles(MakeTwiddles(N, 1)));
         cached[1] = keep.back().get();
      }
      fwd = cached[0];
      inv = cached[1];
   }
   // Lines are independent (each with its own buffers), so they run on any
   // number of threads with the same result.
   auto forLines = [&](const std::function<void(int, std::vector<double>&, std::vector<double>&)>& line) {
      if (!parallel)
      {
         std::vector<double> re(N), im(N);
         for (int k = 0; k < n; ++k)
            line(k, re, im);
         return;
      }
      const unsigned chunks = 16;
      ParallelFor(chunks, [&](unsigned c) {
         std::vector<double> re(N), im(N);
         for (int k = static_cast<int>(c * n / chunks); k < static_cast<int>((c + 1) * n / chunks); ++k)
            line(k, re, im);
      });
   };

   // Row pass: n rows, each zero-padded to N; keep the first n (complex) outputs.
   std::vector<double> midRe(static_cast<size_t>(n) * n), midIm(static_cast<size_t>(n) * n);
   forLines([&](int y, std::vector<double>& re, std::vector<double>& im) {
      std::fill(re.begin(), re.end(), 0.0);
      std::fill(im.begin(), im.end(), 0.0);
      for (int x = 0; x < n; ++x)
         re[x] = kernel[static_cast<size_t>(y) * n + x];
      FourierShiftLine(re, im, N, xc, xs, *fwd, *inv);
      for (int x = 0; x < n; ++x)
      {
         midRe[static_cast<size_t>(y) * n + x] = re[x];
         midIm[static_cast<size_t>(y) * n + x] = im[x];
      }
   });
   // Column pass on the complex intermediate; the real part is the answer.
   std::vector<float> out(static_cast<size_t>(n) * n);
   forLines([&](int x, std::vector<double>& re, std::vector<double>& im) {
      std::fill(re.begin(), re.end(), 0.0);
      std::fill(im.begin(), im.end(), 0.0);
      for (int y = 0; y < n; ++y)
      {
         re[y] = midRe[static_cast<size_t>(y) * n + x];
         im[y] = midIm[static_cast<size_t>(y) * n + x];
      }
      FourierShiftLine(re, im, N, yc, ys, *fwd, *inv);
      for (int y = 0; y < n; ++y)
         out[static_cast<size_t>(y) * n + x] = static_cast<float>(re[y]);
   });
   return out;
}

} // namespace

SplatSetupResult SplatSetup(const PsfKernelCache& cache, double xPx, double yPx, PsfInterpMode interpMode)
{
   // tx,ty: kernel-grid coordinate of camera pixel (x0,y0)'s first sub-cell
   // centre, mapped straight from the emitter position (never rounded to a
   // whole camera pixel first -- that is what gives genuine sub-pixel
   // placement). Pixel (x0+dx)'s is tx + dx*os, an integer step away, so the
   // fraction and every tap weight are shared by the whole splat.
   SplatSetupResult r;
   const int os = std::max(1, cache.oversampling);
   const double kc = (cache.sizeOversampled - 1) / 2.0;
   r.x0 = static_cast<int>(RoundHalfUp(xPx));
   r.y0 = static_cast<int>(RoundHalfUp(yPx));
   const double tx = kc + (r.x0 - xPx - 0.5) * os + 0.5;
   const double ty = kc + (r.y0 - yPx - 0.5) * os + 0.5;
   if (interpMode == PsfInterpMode::Nearest || interpMode == PsfInterpMode::Fft)
   {
      r.bx = static_cast<int>(std::floor(tx + 0.5));
      r.by = static_cast<int>(std::floor(ty + 0.5));
      r.nTaps = 1;
      return r;
   }
   const int bx = static_cast<int>(std::floor(tx)), by = static_cast<int>(std::floor(ty));
   const double fx = tx - bx, fy = ty - by;
   if (interpMode == PsfInterpMode::Cubic)
   {
      r.bx = bx - 1; // taps -1..2
      r.by = by - 1;
      r.nTaps = 4;
      CatmullRomWeights(fx, r.wx);
      CatmullRomWeights(fy, r.wy);
      return r;
   }
   r.bx = bx;
   r.by = by;
   r.nTaps = 2;
   r.wx[0] = 1.0 - fx;
   r.wx[1] = fx;
   r.wy[0] = 1.0 - fy;
   r.wy[1] = fy;
   return r;
}

bool PlanSplat(const PsfKernelCache& cache, int zIndex, double xPx, double yPx, double totalPhotons,
               PsfInterpMode interpMode, SplatPlan& plan, bool parallelFft)
{
   if (!cache.valid || totalPhotons <= 0.0)
      return false;
   if (zIndex < 0 || zIndex >= cache.nz || cache.blockSums.size() != static_cast<size_t>(cache.nz))
      return false;

   const int os = std::max(1, cache.oversampling);
   const int n = cache.sizeOversampled;
   plan.B = cache.blockSums[static_cast<size_t>(zIndex)].data();
   plan.P = nullptr;
   if (cache.polySums.size() == static_cast<size_t>(cache.nz) && !cache.polySums[static_cast<size_t>(zIndex)].empty())
      plan.P = cache.polySums[static_cast<size_t>(zIndex)].data();
   if (interpMode == PsfInterpMode::Fft)
   {
      // Align the shared sub-cell fraction onto the grid with ONE Fourier
      // shift of the raw kernel, then read its block sums nearest.
      plan.st = SplatSetup(cache, xPx, yPx, PsfInterpMode::Nearest);
      const double kc = (n - 1) / 2.0;
      const double tx = kc + (plan.st.x0 - xPx - 0.5) * os + 0.5, ty = kc + (plan.st.y0 - yPx - 0.5) * os + 0.5;
      const double rx = RoundHalfUp(tx), ry = RoundHalfUp(ty);
      std::vector<float> shifted =
         FftShiftKernelTile(cache.planes[static_cast<size_t>(zIndex)].data(), n, tx - rx, ty - ry, parallelFft);
      plan.shiftedSums = BuildBlockSums(shifted.data(), n, os);
      plan.B = plan.shiftedSums.data();
      plan.P = nullptr;
      plan.st.bx = static_cast<int>(rx);
      plan.st.by = static_cast<int>(ry);
   }
   else
   {
      plan.st = SplatSetup(cache, xPx, yPx, interpMode);
   }
   return true;
}

void SplatRows(std::vector<float>& img, unsigned width, unsigned height, int rowLo, int rowHi,
               const PsfKernelCache& cache, const SplatPlan& plan, double totalPhotons)
{
   // The per-pixel arithmetic lives in SplatKernel.inl (two copies: the
   // baseline instruction set and AVX2, chosen here at run time; identical
   // results, ctest sr_render).
   SplatArgs a;
   a.img = img.data();
   a.width = width;
   a.yLo = std::max(0, rowLo);
   a.yHi = std::min(static_cast<int>(height), rowHi);
   a.os = std::max(1, cache.oversampling);
   a.camRad = cache.halfWidthOversampled / a.os;
   a.bw = cache.blockSumWidth;
   a.qw = a.bw % a.os == 0 ? a.bw / a.os : 0;
   a.B = plan.B;
   a.P = a.qw > 0 ? plan.P : nullptr;
   a.x0 = plan.st.x0;
   a.y0 = plan.st.y0;
   a.bx = plan.st.bx;
   a.by = plan.st.by;
   a.nTaps = plan.st.nTaps;
   a.wx = plan.st.wx;
   a.wy = plan.st.wy;
   a.photons = totalPhotons;
   if (splat_avx2::Available())
      splat_avx2::SplatRows(a);
   else
      splat_sse2::SplatRows(a);
}

void SplatPsfKernel(std::vector<float>& img, unsigned width, unsigned height, const PsfKernelCache& cache,
                     int zIndex, double xPx, double yPx, double totalPhotons, PsfInterpMode interpMode)
{
   SplatPlan plan;
   if (PlanSplat(cache, zIndex, xPx, yPx, totalPhotons, interpMode, plan))
      SplatRows(img, width, height, 0, static_cast<int>(height), cache, plan, totalPhotons);
}

namespace {

// Solves the n x n system A x = b by Gaussian elimination with partial
// pivoting (A, b copied). Returns false if A is (numerically) singular.
bool SolveLinear(std::vector<std::vector<double>> a, std::vector<double> b, std::vector<double>& x)
{
   const size_t n = b.size();
   for (size_t c = 0; c < n; ++c)
   {
      size_t piv = c;
      for (size_t r = c + 1; r < n; ++r)
         if (std::fabs(a[r][c]) > std::fabs(a[piv][c]))
            piv = r;
      if (!(std::fabs(a[piv][c]) > 1e-300))
         return false;
      std::swap(a[c], a[piv]);
      std::swap(b[c], b[piv]);
      for (size_t r = c + 1; r < n; ++r)
      {
         double f = a[r][c] / a[c][c];
         for (size_t k = c; k < n; ++k)
            a[r][k] -= f * a[c][k];
         b[r] -= f * b[c];
      }
   }
   x.assign(n, 0.0);
   for (size_t i = n; i-- > 0;)
   {
      double acc = b[i];
      for (size_t k = i + 1; k < n; ++k)
         acc -= a[i][k] * x[k];
      x[i] = acc / a[i][i];
   }
   return true;
}

struct CrlbPlane
{
   double zNm, xNm, yNm, zSigNm;
};

} // namespace

std::string DescribePsfCramerRao(const PsfKernelCache& cache, double photons, double bgPerPx, double cameraPxNm)
{
   if (!cache.valid || cache.nz < 3)
      return std::string();
   const int os = std::max(1, cache.oversampling);
   const int n = cache.sizeOversampled;
   const int bw = n / os, bh = n / os;
   if (bw < 5 || bh < 5)
      return std::string();

   // Camera-pixel PSF per plane, mass-normalized.
   std::vector<std::vector<double>> binned(static_cast<size_t>(cache.nz),
                                           std::vector<double>(static_cast<size_t>(bw) * bh, 0.0));
   for (int k = 0; k < cache.nz; ++k)
   {
      const std::vector<float>& s = cache.planes[static_cast<size_t>(k)];
      std::vector<double>& img = binned[static_cast<size_t>(k)];
      for (int y = 0; y < bh * os; ++y)
         for (int x = 0; x < bw * os; ++x)
            img[static_cast<size_t>(y / os) * bw + x / os] += s[static_cast<size_t>(y) * n + x];
      double sum = 0.0;
      for (double v : img)
         sum += v;
      if (sum > 0.0)
         for (double& v : img)
            v /= sum;
   }

   const double N = std::max(1.0, photons), bg = std::max(1e-6, bgPerPx), dz = cache.zStepNm;
   const int focus = cache.nz / 2;
   const double nan = std::nan("");
   std::vector<CrlbPlane> out;
   for (int k = 1; k < cache.nz - 1; ++k)
   {
      const std::vector<double>& p = binned[static_cast<size_t>(k)];
      const std::vector<double>& pz = binned[static_cast<size_t>(k + 1)];
      const std::vector<double>& pm = binned[static_cast<size_t>(k - 1)];
      std::vector<std::vector<double>> F(5, std::vector<double>(5, 0.0));
      for (int y = 1; y < bh - 1; ++y)
         for (int x = 1; x < bw - 1; ++x)
         {
            const size_t i = static_cast<size_t>(y) * bw + x;
            const double mu = N * p[i] + bg;
            if (!(mu > 0.0))
               continue;
            // d/dx of the MODEL is -N*dp/dx: moving the emitter right moves the pattern right.
            const double du[5] = {-N * (p[i + 1] - p[i - 1]) / 2.0, -N * (p[i + bw] - p[i - bw]) / 2.0, p[i], 1.0,
                                  N * (pz[i] - pm[i]) / (2.0 * dz)};
            for (int a = 0; a < 5; ++a)
               for (int b = a; b < 5; ++b)
               {
                  double v = du[a] * du[b] / mu;
                  F[a][b] += v;
                  if (b != a)
                     F[b][a] += v;
               }
         }
      std::vector<double> ex, ey, ez;
      bool okx = SolveLinear(F, {1, 0, 0, 0, 0}, ex);
      bool oky = SolveLinear(F, {0, 1, 0, 0, 0}, ey);
      bool okz = SolveLinear(F, {0, 0, 0, 0, 1}, ez);
      out.push_back({(k - focus) * dz, okx && ex[0] > 0 ? std::sqrt(ex[0]) * cameraPxNm : nan,
                     oky && ey[1] > 0 ? std::sqrt(ey[1]) * cameraPxNm : nan,
                     okz && ez[4] > 0 ? std::sqrt(ez[4]) : nan});
   }

   const CrlbPlane* best = nullptr;
   const CrlbPlane* atFocus = nullptr;
   for (const CrlbPlane& c : out)
   {
      if (!std::isfinite(c.zSigNm))
         continue;
      if (!best || c.zSigNm < best->zSigNm)
         best = &c;
      if (!atFocus || std::fabs(c.zNm) < std::fabs(atFocus->zNm))
         atFocus = &c;
   }
   if (!best)
      return std::string();

   // Widest contiguous span whose z bound stays within 3x the best -- quoting
   // only the best figure would flatter a PSF that is excellent in one plane
   // and blind everywhere else (which is exactly what an unaberrated PSF is).
   const double lim = 3.0 * best->zSigNm;
   int run = -1;
   bool haveSpan = false;
   double span0 = 0.0, span1 = 0.0;
   for (size_t i = 0; i <= out.size(); ++i)
   {
      bool ok = i < out.size() && std::isfinite(out[i].zSigNm) && out[i].zSigNm <= lim;
      if (ok && run < 0)
         run = static_cast<int>(i);
      if (!ok && run >= 0)
      {
         double z0 = out[static_cast<size_t>(run)].zNm, z1 = out[i - 1].zNm;
         if (!haveSpan || (z1 - z0) > (span1 - span0))
         {
            span0 = z0;
            span1 = z1;
            haveSpan = true;
         }
         run = -1;
      }
   }

   std::ostringstream msg;
   msg << std::fixed << std::setprecision(1) << "PSF depth information (Cramer-Rao bound at " << N << " photons, "
       << bg << " bg photons/px): best sigma_z " << best->zSigNm << " nm at z=" << std::setprecision(0) << best->zNm
       << " nm, " << std::setprecision(1) << atFocus->zSigNm << " nm at focus (lateral " << atFocus->xNm << " nm)";
   if (haveSpan)
      msg << std::setprecision(0) << ", staying within 3x of best over " << span0 << ".." << span1 << " nm";
   msg << ". No fitter can do better than this -- it is the yardstick, not a measurement of any fit.";
   return msg.str();
}

} // namespace sim
