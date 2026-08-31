#include "PsfGeneratorBridge.h"
#include "PsfResource.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>

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

int PsfKernelCache::NearestZIndex(double zUm) const
{
   if (nz <= 1 || zStepNm <= 0.0)
      return 0;
   double idxF = zUm * 1000.0 / zStepNm + (nz - 1) / 2.0;
   int idx = static_cast<int>(std::lround(idxF));
   if (idx < 0)
      idx = 0;
   if (idx >= nz)
      idx = nz - 1;
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

// Directory containing this DLL's own file (mmgr_dal_SMLMDemoCam.dll),
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

// Extracts the embedded PSFGenerator+bridge jar (SMLMDemoCam.rc, resource
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
   std::string tempPath = std::string(tempDir) + "SMLMDemoCam_PsfGenerator.jar";

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
   // logged, when selecting a vectorial PsfModel). If jvm.dll is already
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
// SMLMDemoCam.rc / ExtractEmbeddedJar above) and caches it as a global ref.
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

bool ComputePsfKernelCache(const PsfGeneratorRequest& req, PsfKernelCache& outCache, std::string& outError)
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
      // together into the single jar embedded in this DLL (SMLMDemoCam.rc,
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
         env->GetStaticMethodID(cls, "computePlanes", "(Ljava/lang/String;DDDDDIII)[F");
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
      jobject result = env->CallStaticObjectMethod(cls, method, modelStr, static_cast<jdouble>(req.na),
                                                     static_cast<jdouble>(req.wavelengthNm),
                                                     static_cast<jdouble>(req.immersionIndex),
                                                     static_cast<jdouble>(resLateralNm),
                                                     static_cast<jdouble>(req.zStepNm), static_cast<jint>(size),
                                                     static_cast<jint>(size), static_cast<jint>(nz));
      env->DeleteLocalRef(modelStr);
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
      outCache.planes.assign(static_cast<size_t>(nz), std::vector<float>(planeFloats));
      for (int z = 0; z < nz; ++z)
         std::memcpy(outCache.planes[static_cast<size_t>(z)].data(), flat.data() + static_cast<size_t>(z) * planeFloats,
                     planeFloats * sizeof(float));
      outCache.valid = true;
      ok = true;
   } while (false);

   g_jvm->DetachCurrentThread();
   return ok;
}

#else // !_WIN32

bool ComputePsfKernelCache(const PsfGeneratorRequest&, PsfKernelCache& outCache, std::string& outError)
{
   outCache = PsfKernelCache();
   outError = "Embedded PSFGenerator JVM bridge is only implemented for Windows.";
   return false;
}

#endif

void SplatPsfKernel(std::vector<float>& img, unsigned width, unsigned height, const PsfKernelCache& cache,
                     int zIndex, double xPx, double yPx, double totalPhotons)
{
   if (!cache.valid || totalPhotons <= 0.0)
      return;
   if (zIndex < 0 || zIndex >= cache.nz)
      return;

   const int over = std::max(1, cache.oversampling);
   const int half = cache.halfWidthOversampled;
   const int size = cache.sizeOversampled;
   const int camHalf = half / over;
   const std::vector<float>& plane = cache.planes[static_cast<size_t>(zIndex)];

   const int cx = static_cast<int>(std::lround(xPx));
   const int cy = static_cast<int>(std::lround(yPx));
   const double fracX = xPx - cx; // in [-0.5, 0.5)
   const double fracY = yPx - cy;

   const int kernelSide = 2 * camHalf + 1;
   std::vector<double> kernelVals(static_cast<size_t>(kernelSide) * kernelSide, 0.0);
   double sum = 0.0;
   int idx = 0;
   for (int dy = -camHalf; dy <= camHalf; ++dy)
   {
      // Oversampled-index window covering camera-pixel offset dy, shifted
      // to account for the emitter's fractional-pixel position.
      double ovCenterY = half + (dy - fracY) * over;
      int ovLoY = static_cast<int>(std::floor(ovCenterY - over / 2.0 + 0.5));
      for (int dx = -camHalf; dx <= camHalf; ++dx, ++idx)
      {
         double ovCenterX = half + (dx - fracX) * over;
         int ovLoX = static_cast<int>(std::floor(ovCenterX - over / 2.0 + 0.5));

         double acc = 0.0;
         int cnt = 0;
         for (int oy = 0; oy < over; ++oy)
         {
            int sy = ovLoY + oy;
            if (sy < 0 || sy >= size)
               continue;
            const float* row = plane.data() + static_cast<size_t>(sy) * size;
            for (int ox = 0; ox < over; ++ox)
            {
               int sx = ovLoX + ox;
               if (sx < 0 || sx >= size)
                  continue;
               acc += row[sx];
               ++cnt;
            }
         }
         double v = cnt > 0 ? acc / cnt : 0.0;
         kernelVals[static_cast<size_t>(idx)] = v;
         sum += v;
      }
   }
   if (sum <= 0.0)
      return;

   idx = 0;
   for (int dy = -camHalf; dy <= camHalf; ++dy)
   {
      int py = cy + dy;
      if (py < 0 || py >= static_cast<int>(height))
      {
         idx += kernelSide;
         continue;
      }
      float* row = img.data() + static_cast<size_t>(py) * width;
      for (int dx = -camHalf; dx <= camHalf; ++dx, ++idx)
      {
         int px = cx + dx;
         if (px < 0 || px >= static_cast<int>(width))
            continue;
         row[px] += static_cast<float>(kernelVals[static_cast<size_t>(idx)] / sum * totalPhotons);
      }
   }
}

} // namespace sim
