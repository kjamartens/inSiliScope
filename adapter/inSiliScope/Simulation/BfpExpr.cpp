///////////////////////////////////////////////////////////////////////////////
// FILE:          BfpExpr.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See BfpExpr.h: a recursive-descent parser to a stack program.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "BfpExpr.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace sim {

namespace {
constexpr double kPi = 3.14159265358979323846;

struct Fn
{
   const char* name;
   int minArgs, maxArgs;   // maxArgs -1: any
};
// The functions, in their OP_CALL index order.
const Fn kFns[] = {
   { "exp", 1, 1 },   { "log", 1, 1 },   { "sqrt", 1, 1 },  { "abs", 1, 1 },   { "sin", 1, 1 },  { "cos", 1, 1 },
   { "tan", 1, 1 },   { "asin", 1, 1 },  { "acos", 1, 1 },  { "atan", 1, 1 },  { "atan2", 2, 2 }, { "hypot", 2, 2 },
   { "min", 1, -1 },  { "max", 1, -1 },  { "pow", 2, 2 },   { "floor", 1, 1 }, { "ceil", 1, 1 }, { "round", 1, 1 },
   { "mod", 2, 2 },   { "step", 1, 1 },  { "gauss", 2, 2 }, { "spot", 3, 3 },  { "clamp", 3, 3 }, { "if", 3, 3 },
};
constexpr int kFnCount = static_cast<int>(sizeof(kFns) / sizeof(kFns[0]));

int FnIndex(const std::string& n)
{
   for (int i = 0; i < kFnCount; ++i)
      if (n == kFns[i].name)
         return i;
   return -1;
}

double CallFn(int f, const double* a, int n)
{
   switch (f)
   {
   case 0: return std::exp(a[0]);
   case 1: return std::log(a[0]);
   case 2: return std::sqrt(a[0]);
   case 3: return std::fabs(a[0]);
   case 4: return std::sin(a[0]);
   case 5: return std::cos(a[0]);
   case 6: return std::tan(a[0]);
   case 7: return std::asin(a[0]);
   case 8: return std::acos(a[0]);
   case 9: return std::atan(a[0]);
   case 10: return std::atan2(a[0], a[1]);
   case 11: return std::hypot(a[0], a[1]);
   case 12:
   {
      double v = a[0];
      for (int i = 1; i < n; ++i)
         v = std::min(v, a[i]);
      return v;
   }
   case 13:
   {
      double v = a[0];
      for (int i = 1; i < n; ++i)
         v = std::max(v, a[i]);
      return v;
   }
   case 14: return std::pow(a[0], a[1]);
   case 15: return std::floor(a[0]);
   case 16: return std::ceil(a[0]);
   case 17: return std::round(a[0]);
   case 18: return a[1] != 0.0 ? a[0] - a[1] * std::floor(a[0] / a[1]) : 0.0;
   case 19: return a[0] >= 0.0 ? 1.0 : 0.0;
   case 20: return a[1] != 0.0 ? std::exp(-a[0] * a[0] / (2.0 * a[1] * a[1])) : (a[0] == 0.0 ? 1.0 : 0.0);
   case 21: return a[2] != 0.0 ? std::exp(-2.0 * (a[0] * a[0] + a[1] * a[1]) / (a[2] * a[2])) : 0.0;
   case 22: return std::min(std::max(a[0], a[1]), a[2]);
   case 23: return a[0] != 0.0 ? a[1] : a[2];
   default: return 0.0;
   }
}

enum Tok { T_END, T_NUM, T_ID, T_STR, T_OP, T_SEP, T_LP, T_RP, T_COMMA, T_SEMI };
struct Token
{
   Tok t;
   std::string s;
   double v = 0;
   int line = 1;
};

bool Tokenize(const std::string& text, std::vector<Token>& out, std::string& err)
{
   int line = 1, depth = 0;
   size_t i = 0;
   auto push = [&](Tok t, const std::string& s, double v = 0) { out.push_back({ t, s, v, line }); };
   while (i < text.size())
   {
      const char c = text[i];
      if (c == '#')
      {
         while (i < text.size() && text[i] != '\n')
            i++;
         continue;
      }
      if (c == '\n')
      {
         if (depth == 0)
            push(T_SEP, "\n");
         line++;
         i++;
         continue;
      }
      if (c == ' ' || c == '\t' || c == '\r')
      {
         i++;
         continue;
      }
      if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9'))
      {
         char* end = nullptr;
         const double v = std::strtod(text.c_str() + i, &end);
         const size_t n = static_cast<size_t>(end - (text.c_str() + i));
         push(T_NUM, text.substr(i, n), v);
         i += n;
         continue;
      }
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')
      {
         size_t j = i;
         while (j < text.size() && ((text[j] >= 'A' && text[j] <= 'Z') || (text[j] >= 'a' && text[j] <= 'z') ||
                                    (text[j] >= '0' && text[j] <= '9') || text[j] == '_'))
            j++;
         push(T_ID, text.substr(i, j - i));
         i = j;
         continue;
      }
      if (c == '"' || c == '\'')
      {
         size_t j = i + 1;
         while (j < text.size() && text[j] != c && text[j] != '\n')
            j++;
         if (j >= text.size() || text[j] != c)
         {
            err = "line " + std::to_string(line) + ": an unclosed quote";
            return false;
         }
         push(T_STR, text.substr(i + 1, j - i - 1));
         i = j + 1;
         continue;
      }
      if (c == '(')
      {
         depth++;
         push(T_LP, "(");
         i++;
         continue;
      }
      if (c == ')')
      {
         depth = std::max(0, depth - 1);
         push(T_RP, ")");
         i++;
         continue;
      }
      if (c == ',')
      {
         push(T_COMMA, ",");
         i++;
         continue;
      }
      if (c == ';')
      {
         push(depth == 0 ? T_SEP : T_SEMI, ";");
         i++;
         continue;
      }
      const std::string two = text.substr(i, 2);
      if (two == "<=" || two == ">=" || two == "==" || two == "!=" || two == "&&" || two == "||")
      {
         push(T_OP, two);
         i += 2;
         continue;
      }
      if (std::strchr("+-*/^<>=!", c))
      {
         push(T_OP, std::string(1, c));
         i++;
         continue;
      }
      err = "line " + std::to_string(line) + ": unexpected '" + std::string(1, c) + "'";
      return false;
   }
   push(T_END, "");
   return true;
}

struct Parser
{
   const std::vector<Token>& tk;
   size_t p = 0;
   std::vector<BfpProgram::Instr>& ops;
   std::vector<std::string>& locals;
   std::vector<std::string>& images;
   std::string err;

   const Token& Cur() const { return tk[p]; }
   bool Fail(const std::string& m)
   {
      if (err.empty())
         err = "line " + std::to_string(Cur().line) + ": " + m;
      return false;
   }
   bool IsOp(const char* s) const { return Cur().t == T_OP && Cur().s == s; }
   void Emit(BfpProgram::Op op, int32_t a = 0, int32_t b = 0, double c = 0) { ops.push_back({ op, a, b, c }); }

   int Register(const std::string& n) const
   {
      const auto& vars = BfpVarNames();
      for (size_t i = 0; i < vars.size(); ++i)
         if (n == vars[i])
            return static_cast<int>(i);
      const auto& maps = BfpMapNames();
      for (size_t i = 0; i < maps.size(); ++i)
         if (n == maps[i])
            return kBfpVars + static_cast<int>(i);
      for (size_t i = 0; i < locals.size(); ++i)
         if (n == locals[i])
            return kBfpVars + kBfpMaps + static_cast<int>(i);
      return -1;
   }

   bool Program()
   {
      while (Cur().t != T_END)
      {
         if (Cur().t == T_SEP)
         {
            p++;
            continue;
         }
         if (!Statement())
            return false;
         if (Cur().t != T_SEP && Cur().t != T_END)
            return Fail("';' or a new line expected after a statement, not '" + Cur().s + "'");
      }
      return true;
   }
   bool Statement()
   {
      if (Cur().t != T_ID)
         return Fail("a statement 'name = expression' expected, not '" + Cur().s + "'");
      const std::string name = Cur().s;
      p++;
      if (!IsOp("="))
         return Fail("'=' expected after '" + name + "'");
      p++;
      if (!Expr())
         return false;
      int r = Register(name);
      if (r >= 0 && r < kBfpVars)
         return Fail("'" + name + "' is a variable and cannot be set");
      if (FnIndex(name) >= 0 || name == "img")
         return Fail("'" + name + "' is a function and cannot be set");
      if (r < 0)
      {
         locals.push_back(name);
         r = Register(name);
      }
      Emit(BfpProgram::OP_STORE, r);
      return true;
   }
   bool Expr() { return Or(); }
   bool Or()
   {
      if (!And())
         return false;
      while (IsOp("||"))
      {
         p++;
         if (!And())
            return false;
         Emit(BfpProgram::OP_OR);
      }
      return true;
   }
   bool And()
   {
      if (!Cmp())
         return false;
      while (IsOp("&&"))
      {
         p++;
         if (!Cmp())
            return false;
         Emit(BfpProgram::OP_AND);
      }
      return true;
   }
   bool Cmp()
   {
      if (!Add())
         return false;
      static const char* ops_[] = { "<", "<=", ">", ">=", "==", "!=" };
      static const BfpProgram::Op codes[] = { BfpProgram::OP_LT, BfpProgram::OP_LE, BfpProgram::OP_GT,
                                              BfpProgram::OP_GE, BfpProgram::OP_EQ, BfpProgram::OP_NE };
      for (int k = 0; k < 6; ++k)
         if (IsOp(ops_[k]))
         {
            p++;
            if (!Add())
               return false;
            Emit(codes[k]);
            return true;
         }
      return true;
   }
   bool Add()
   {
      if (!Mul())
         return false;
      while (IsOp("+") || IsOp("-"))
      {
         const bool plus = IsOp("+");
         p++;
         if (!Mul())
            return false;
         Emit(plus ? BfpProgram::OP_ADD : BfpProgram::OP_SUB);
      }
      return true;
   }
   bool Mul()
   {
      if (!Unary())
         return false;
      while (IsOp("*") || IsOp("/"))
      {
         const bool times = IsOp("*");
         p++;
         if (!Unary())
            return false;
         Emit(times ? BfpProgram::OP_MUL : BfpProgram::OP_DIV);
      }
      return true;
   }
   bool Unary()
   {
      if (IsOp("-") || IsOp("+") || IsOp("!"))
      {
         const std::string o = Cur().s;
         p++;
         if (!Unary())
            return false;
         if (o == "-")
            Emit(BfpProgram::OP_NEG);
         else if (o == "!")
            Emit(BfpProgram::OP_NOT);
         return true;
      }
      return Pow();
   }
   bool Pow()
   {
      if (!Primary())
         return false;
      if (IsOp("^"))
      {
         p++;
         if (!Unary())   // right associative; -2^2 = -(2^2) (Unary took the sign first)
            return false;
         Emit(BfpProgram::OP_POW);
      }
      return true;
   }
   bool Primary()
   {
      const Token& t = Cur();
      if (t.t == T_NUM)
      {
         Emit(BfpProgram::OP_CONST, 0, 0, t.v);
         p++;
         return true;
      }
      if (t.t == T_LP)
      {
         p++;
         if (!Expr())
            return false;
         if (Cur().t != T_RP)
            return Fail("')' expected");
         p++;
         return true;
      }
      if (t.t == T_ID)
      {
         const std::string n = t.s;
         p++;
         if (Cur().t == T_LP)
         {
            p++;
            if (n == "img")
            {
               if (Cur().t != T_STR)
                  return Fail("img(\"file\") takes a file name in quotes");
               const std::string file = Cur().s;
               p++;
               if (Cur().t != T_RP)
                  return Fail("')' expected after img's file name");
               p++;
               int k = -1;
               for (size_t i = 0; i < images.size(); ++i)
                  if (images[i] == file)
                     k = static_cast<int>(i);
               if (k < 0)
               {
                  images.push_back(file);
                  k = static_cast<int>(images.size()) - 1;
               }
               Emit(BfpProgram::OP_IMG, k);
               return true;
            }
            const int f = FnIndex(n);
            if (f < 0)
               return Fail("unknown function '" + n + "'");
            int args = 0;
            if (Cur().t != T_RP)
               for (;;)
               {
                  if (!Expr())
                     return false;
                  args++;
                  if (Cur().t == T_COMMA || Cur().t == T_SEMI)
                  {
                     p++;
                     continue;
                  }
                  break;
               }
            if (Cur().t != T_RP)
               return Fail("')' or ',' expected in " + n + "(...)");
            p++;
            if (args < kFns[f].minArgs || (kFns[f].maxArgs >= 0 && args > kFns[f].maxArgs))
               return Fail(n + "() takes " + std::to_string(kFns[f].minArgs) +
                           (kFns[f].maxArgs == kFns[f].minArgs ? "" : kFns[f].maxArgs < 0 ? " or more" : " to " + std::to_string(kFns[f].maxArgs)) +
                           " arguments, not " + std::to_string(args));
            Emit(BfpProgram::OP_CALL, f, args);
            return true;
         }
         const int r = Register(n);
         if (r < 0)
            return Fail("unknown name '" + n + "' (set it first, or see the variables in the help)");
         Emit(BfpProgram::OP_LOAD, r);
         return true;
      }
      if (t.t == T_STR)
         return Fail("a quoted name is only img(\"file\")'s");
      return Fail(t.t == T_END || t.t == T_SEP ? std::string("an expression expected") : "unexpected '" + t.s + "'");
   }
};
} // namespace

const std::vector<std::string>& BfpVarNames()
{
   static const std::vector<std::string> v = { "ux", "uy", "r",  "phi", "nm", "t",  "umax",   "uc",     "pi",
                                               "bfp", "w", "fwhm", "ns", "ni", "na", "f", "sheetT", "sheetW",
                                               "unpolarized" };
   return v;
}

const std::vector<std::string>& BfpMapNames()
{
   static const std::vector<std::string> v = { "I",     "phase", "pol",      "polLab", "ellip", "tiltX", "tiltY",
                                               "focus", "coherent", "T",     "duty",   "scanX", "scanY", "scanRot" };
   return v;
}

double BfpMapDefault(int map)
{
   switch (map)
   {
   case BM_COHERENT:
   case BM_DUTY: return 1.0;
   case BM_POL: return kBfpUnpolarized;
   case BM_POL_LAB: return std::numeric_limits<double>::quiet_NaN();   // not set: pol applies
   default: return 0.0;
   }
}

bool BfpProgram::Compile(const std::string& text, std::string& err)
{
   ops_.clear();
   locals_.clear();
   images_.clear();
   setMask_ = 0;
   std::vector<Token> tk;
   if (!Tokenize(text, tk, err))
      return false;
   Parser P{ tk, 0, ops_, locals_, images_, std::string() };
   if (!P.Program())
   {
      err = P.err;
      ops_.clear();
      return false;
   }
   registers_ = kBfpVars + kBfpMaps + static_cast<int>(locals_.size());
   for (const Instr& in : ops_)
      if (in.op == OP_STORE && in.a >= kBfpVars && in.a < kBfpVars + kBfpMaps)
         setMask_ |= 1u << (in.a - kBfpVars);
   return true;
}

unsigned BfpProgram::DependsOn(int map, const std::vector<int>& imagePages) const
{
   std::vector<unsigned> reg(static_cast<size_t>(std::max(registers_, kBfpVars + kBfpMaps)), 0u);
   reg[BV_UX] = reg[BV_UY] = reg[BV_R] = reg[BV_PHI] = BD_U;
   reg[BV_T] = BD_T;
   reg[BV_NM] = reg[BV_W] = BD_NM;
   std::vector<unsigned> st;
   for (const Instr& in : ops_)
   {
      switch (in.op)
      {
      case OP_CONST: st.push_back(0u); break;
      case OP_LOAD: st.push_back(reg[static_cast<size_t>(in.a)]); break;
      case OP_STORE:
         reg[static_cast<size_t>(in.a)] = st.back();
         st.pop_back();
         break;
      case OP_NEG:
      case OP_NOT: break;
      case OP_CALL:
      {
         unsigned m = 0;
         for (int k = 0; k < in.b; ++k)
         {
            m |= st.back();
            st.pop_back();
         }
         st.push_back(m);
         break;
      }
      case OP_IMG:
      {
         const int pages = in.a < static_cast<int>(imagePages.size()) ? imagePages[static_cast<size_t>(in.a)] : 2;
         st.push_back(BD_U | (pages > 1 ? BD_T : 0u));
         break;
      }
      default:
      {
         const unsigned b = st.back();
         st.pop_back();
         st.back() |= b;
         break;
      }
      }
   }
   return reg[static_cast<size_t>(kBfpVars + map)];
}

void BfpProgram::Run(const double* vars, double* maps, std::vector<double>& R, const ImageSampler* images) const
{
   R.resize(static_cast<size_t>(std::max(registers_, kBfpVars + kBfpMaps)));
   std::copy(vars, vars + kBfpVars, R.begin());
   for (int m = 0; m < kBfpMaps; ++m)
      R[static_cast<size_t>(kBfpVars + m)] = BfpMapDefault(m);
   double st[64];
   int sp = 0;
   double args[64];
   for (const Instr& in : ops_)
   {
      switch (in.op)
      {
      case OP_CONST: st[sp++] = in.c; break;
      case OP_LOAD: st[sp++] = R[static_cast<size_t>(in.a)]; break;
      case OP_STORE: R[static_cast<size_t>(in.a)] = st[--sp]; break;
      case OP_NEG: st[sp - 1] = -st[sp - 1]; break;
      case OP_NOT: st[sp - 1] = st[sp - 1] != 0.0 ? 0.0 : 1.0; break;
      case OP_CALL:
      {
         const int n = std::min(in.b, 64);
         for (int k = n - 1; k >= 0; --k)
            args[k] = st[--sp];
         st[sp++] = CallFn(in.a, args, n);
         break;
      }
      case OP_IMG: st[sp++] = images ? images->Sample(in.a, vars[BV_UX], vars[BV_UY], vars[BV_T]) : 0.0; break;
      default:
      {
         const double b = st[--sp], a = st[sp - 1];
         double v = 0.0;
         switch (in.op)
         {
         case OP_ADD: v = a + b; break;
         case OP_SUB: v = a - b; break;
         case OP_MUL: v = a * b; break;
         case OP_DIV: v = a / b; break;
         case OP_POW: v = std::pow(a, b); break;
         case OP_LT: v = a < b ? 1.0 : 0.0; break;
         case OP_LE: v = a <= b ? 1.0 : 0.0; break;
         case OP_GT: v = a > b ? 1.0 : 0.0; break;
         case OP_GE: v = a >= b ? 1.0 : 0.0; break;
         case OP_EQ: v = a == b ? 1.0 : 0.0; break;
         case OP_NE: v = a != b ? 1.0 : 0.0; break;
         case OP_AND: v = (a != 0.0 && b != 0.0) ? 1.0 : 0.0; break;
         case OP_OR: v = (a != 0.0 || b != 0.0) ? 1.0 : 0.0; break;
         default: break;
         }
         st[sp - 1] = v;
         break;
      }
      }
      if (sp >= 63)
         sp = 63;   // (a pathological depth: the parser's nesting bounds it in practice)
   }
   std::copy(R.begin() + kBfpVars, R.begin() + kBfpVars + kBfpMaps, maps);
}

std::string BfpOneLine(const std::string& text)
{
   std::string out, cur;
   bool quoted = false, comment = false;
   auto flush = [&]() {
      const size_t a = cur.find_first_not_of(" \t\r"), b = cur.find_last_not_of(" \t\r");
      if (a != std::string::npos)
         out += (out.empty() ? "" : "; ") + cur.substr(a, b - a + 1);
      cur.clear();
   };
   for (char c : text)
   {
      if (c == '\n')
      {
         flush();
         comment = false;
         quoted = false;
         continue;
      }
      if (comment)
         continue;   // (a ';' in a comment too)
      if (c == ';' && !quoted)
      {
         flush();
         continue;
      }
      if (c == '"')
         quoted = !quoted;
      else if (c == '#' && !quoted)
      {
         comment = true;
         continue;
      }
      cur += c;
   }
   flush();
   return out;
}

} // namespace sim
