// paws - Universal FSR 1.0 Spatial Upscaler (Linux)
// Zero third-party wrapper libraries: X11 (libX11, libXcomposite, libXfixes) + OpenGL/GLX + GLSL FSR 1.0 (EASU + RCAS).

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>

#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

volatile sig_atomic_t g_ShouldQuit = 0;
bool g_X11ErrorOccurred = false;

void HandleSignal(int) {
    g_ShouldQuit = 1;
}

int HandleX11Error(Display*, XErrorEvent*) {
    g_X11ErrorOccurred = true;
    return 0;
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ============================================================================
// Dynamically loaded GL 3.3 entry points via glXGetProcAddressARB
// ============================================================================
struct GlProcs {
    PFNGLCREATESHADERPROC CreateShader = nullptr;
    PFNGLSHADERSOURCEPROC ShaderSource = nullptr;
    PFNGLCOMPILESHADERPROC CompileShader = nullptr;
    PFNGLGETSHADERIVPROC GetShaderiv = nullptr;
    PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog = nullptr;
    PFNGLCREATEPROGRAMPROC CreateProgram = nullptr;
    PFNGLATTACHSHADERPROC AttachShader = nullptr;
    PFNGLLINKPROGRAMPROC LinkProgram = nullptr;
    PFNGLGETPROGRAMIVPROC GetProgramiv = nullptr;
    PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog = nullptr;
    PFNGLDELETESHADERPROC DeleteShader = nullptr;
    PFNGLDELETEPROGRAMPROC DeleteProgram = nullptr;
    PFNGLUSEPROGRAMPROC UseProgram = nullptr;
    PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation = nullptr;
    PFNGLUNIFORM1IPROC Uniform1i = nullptr;
    PFNGLUNIFORM1FPROC Uniform1f = nullptr;
    PFNGLUNIFORM2FPROC Uniform2f = nullptr;
    PFNGLGENVERTEXARRAYSPROC GenVertexArrays = nullptr;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray = nullptr;
    PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays = nullptr;
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers = nullptr;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = nullptr;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D = nullptr;
    PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers = nullptr;
    PFNGLACTIVETEXTUREPROC ActiveTexture = nullptr;

    bool Load() {
        auto load = [](const char* name) {
            return glXGetProcAddressARB(reinterpret_cast<const GLubyte*>(name));
        };
        CreateShader = reinterpret_cast<PFNGLCREATESHADERPROC>(load("glCreateShader"));
        ShaderSource = reinterpret_cast<PFNGLSHADERSOURCEPROC>(load("glShaderSource"));
        CompileShader = reinterpret_cast<PFNGLCOMPILESHADERPROC>(load("glCompileShader"));
        GetShaderiv = reinterpret_cast<PFNGLGETSHADERIVPROC>(load("glGetShaderiv"));
        GetShaderInfoLog = reinterpret_cast<PFNGLGETSHADERINFOLOGPROC>(load("glGetShaderInfoLog"));
        CreateProgram = reinterpret_cast<PFNGLCREATEPROGRAMPROC>(load("glCreateProgram"));
        AttachShader = reinterpret_cast<PFNGLATTACHSHADERPROC>(load("glAttachShader"));
        LinkProgram = reinterpret_cast<PFNGLLINKPROGRAMPROC>(load("glLinkProgram"));
        GetProgramiv = reinterpret_cast<PFNGLGETPROGRAMIVPROC>(load("glGetProgramiv"));
        GetProgramInfoLog = reinterpret_cast<PFNGLGETPROGRAMINFOLOGPROC>(load("glGetProgramInfoLog"));
        DeleteShader = reinterpret_cast<PFNGLDELETESHADERPROC>(load("glDeleteShader"));
        DeleteProgram = reinterpret_cast<PFNGLDELETEPROGRAMPROC>(load("glDeleteProgram"));
        UseProgram = reinterpret_cast<PFNGLUSEPROGRAMPROC>(load("glUseProgram"));
        GetUniformLocation = reinterpret_cast<PFNGLGETUNIFORMLOCATIONPROC>(load("glGetUniformLocation"));
        Uniform1i = reinterpret_cast<PFNGLUNIFORM1IPROC>(load("glUniform1i"));
        Uniform1f = reinterpret_cast<PFNGLUNIFORM1FPROC>(load("glUniform1f"));
        Uniform2f = reinterpret_cast<PFNGLUNIFORM2FPROC>(load("glUniform2f"));
        GenVertexArrays = reinterpret_cast<PFNGLGENVERTEXARRAYSPROC>(load("glGenVertexArrays"));
        BindVertexArray = reinterpret_cast<PFNGLBINDVERTEXARRAYPROC>(load("glBindVertexArray"));
        DeleteVertexArrays = reinterpret_cast<PFNGLDELETEVERTEXARRAYSPROC>(load("glDeleteVertexArrays"));
        GenFramebuffers = reinterpret_cast<PFNGLGENFRAMEBUFFERSPROC>(load("glGenFramebuffers"));
        BindFramebuffer = reinterpret_cast<PFNGLBINDFRAMEBUFFERPROC>(load("glBindFramebuffer"));
        FramebufferTexture2D = reinterpret_cast<PFNGLFRAMEBUFFERTEXTURE2DPROC>(load("glFramebufferTexture2D"));
        DeleteFramebuffers = reinterpret_cast<PFNGLDELETEFRAMEBUFFERSPROC>(load("glDeleteFramebuffers"));
        ActiveTexture = reinterpret_cast<PFNGLACTIVETEXTUREPROC>(load("glActiveTexture"));

        return CreateShader && ShaderSource && CompileShader && CreateProgram &&
               LinkProgram && UseProgram && GenVertexArrays && GenFramebuffers && ActiveTexture;
    }
};

// ============================================================================
// GLSL Shaders: Fullscreen Triangle VS + FSR 1.0 EASU FS + FSR 1.0 RCAS FS
// ============================================================================
static const char* kFullscreenVS = R"GLSL(
#version 330 core
out vec2 vUV;
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    // Flip Y for X11 top-down image data
    vUV = vec2(pos.x, 1.0 - pos.y);
}
)GLSL";

static const char* kFullscreenNoFlipVS = R"GLSL(
#version 330 core
out vec2 vUV;
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    vUV = pos;
}
)GLSL";

// Pass 1: FSR 1.0 EASU (Edge-Adaptive Spatial Upsampling)
static const char* kFsrEasuFS = R"GLSL(
#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSrcTex;
uniform vec2 uSrcSize;
uniform vec2 uInvSrcSize;

float Luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

float EasuWeight(float d2, float lob, float clp) {
    d2 = min(d2, clp);
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    return wB * wA;
}

void main() {
    vec2 pp = vUV * uSrcSize - 0.5;
    vec2 fp = floor(pp);
    vec2 f  = pp - fp;
    vec2 baseUv = (fp + 0.5) * uInvSrcSize;

    vec3 b  = texture(uSrcTex, baseUv + vec2( 0.0, -1.0) * uInvSrcSize).rgb;
    vec3 c  = texture(uSrcTex, baseUv + vec2( 1.0, -1.0) * uInvSrcSize).rgb;
    vec3 e  = texture(uSrcTex, baseUv + vec2(-1.0,  0.0) * uInvSrcSize).rgb;
    vec3 f0 = texture(uSrcTex, baseUv + vec2( 0.0,  0.0) * uInvSrcSize).rgb;
    vec3 g  = texture(uSrcTex, baseUv + vec2( 1.0,  0.0) * uInvSrcSize).rgb;
    vec3 h  = texture(uSrcTex, baseUv + vec2( 2.0,  0.0) * uInvSrcSize).rgb;
    vec3 i  = texture(uSrcTex, baseUv + vec2(-1.0,  1.0) * uInvSrcSize).rgb;
    vec3 j  = texture(uSrcTex, baseUv + vec2( 0.0,  1.0) * uInvSrcSize).rgb;
    vec3 k  = texture(uSrcTex, baseUv + vec2( 1.0,  1.0) * uInvSrcSize).rgb;
    vec3 l  = texture(uSrcTex, baseUv + vec2( 2.0,  1.0) * uInvSrcSize).rgb;
    vec3 n  = texture(uSrcTex, baseUv + vec2( 0.0,  2.0) * uInvSrcSize).rgb;
    vec3 o  = texture(uSrcTex, baseUv + vec2( 1.0,  2.0) * uInvSrcSize).rgb;

    float lb = Luma(b), lc = Luma(c), le = Luma(e), lf = Luma(f0);
    float lg = Luma(g), lh = Luma(h), li = Luma(i), lj = Luma(j);
    float lk = Luma(k), ll = Luma(l), ln = Luma(n), lo = Luma(o);

    float w0 = (1.0 - f.x) * (1.0 - f.y);
    float w1 = f.x * (1.0 - f.y);
    float w2 = (1.0 - f.x) * f.y;
    float w3 = f.x * f.y;

    vec2 dir = vec2(lg - le, lj - lb) * w0
             + vec2(lh - lf, lk - lc) * w1
             + vec2(lk - li, ln - lf) * w2
             + vec2(ll - lj, lo - lg) * w3;

    float dirLen2 = dot(dir, dir);
    dir = (dirLen2 > 1e-6) ? (dir * inversesqrt(dirLen2)) : vec2(1.0, 0.0);
    float len = clamp(sqrt(dirLen2) * 2.0, 0.0, 1.0);

    float stretch = 1.0 / max(abs(dir.x), abs(dir.y));
    vec2 aniso = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    vec3 taps[12] = vec3[12](b, c, e, f0, g, h, i, j, k, l, n, o);
    vec2 offs[12] = vec2[12](
        vec2( 0,-1), vec2( 1,-1),
        vec2(-1, 0), vec2( 0, 0), vec2( 1, 0), vec2( 2, 0),
        vec2(-1, 1), vec2( 0, 1), vec2( 1, 1), vec2( 2, 1),
        vec2( 0, 2), vec2( 1, 2)
    );

    vec3 acc = vec3(0.0);
    float wSum = 0.0;
    for (int t = 0; t < 12; ++t) {
        vec2 d = offs[t] - f;
        vec2 rd = vec2(d.x * dir.x + d.y * dir.y, -d.x * dir.y + d.y * dir.x) * aniso;
        float w = EasuWeight(dot(rd, rd), lob, clp);
        acc += taps[t] * w;
        wSum += w;
    }

    vec3 color = acc / max(wSum, 1e-5);
    vec3 min4 = min(min(f0, g), min(j, k));
    vec3 max4 = max(max(f0, g), max(j, k));
    FragColor = vec4(clamp(color, min4, max4), 1.0);
}
)GLSL";

// Pass 2: FSR 1.0 RCAS (Robust Contrast-Adaptive Sharpening)
static const char* kFsrRcasFS = R"GLSL(
#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uEasuTex;
uniform float uSharpness;
uniform int uEnableRcas;

float Luma(vec3 c) {
    return 0.5 * c.g + 0.25 * (c.r + c.b);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 e = texelFetch(uEasuTex, p, 0).rgb;
    if (uEnableRcas == 0) {
        FragColor = vec4(e, 1.0);
        return;
    }

    ivec2 maxCoord = textureSize(uEasuTex, 0) - ivec2(1);
    vec3 b = texelFetch(uEasuTex, clamp(p + ivec2( 0, -1), ivec2(0), maxCoord), 0).rgb;
    vec3 d = texelFetch(uEasuTex, clamp(p + ivec2(-1,  0), ivec2(0), maxCoord), 0).rgb;
    vec3 f = texelFetch(uEasuTex, clamp(p + ivec2( 1,  0), ivec2(0), maxCoord), 0).rgb;
    vec3 h = texelFetch(uEasuTex, clamp(p + ivec2( 0,  1), ivec2(0), maxCoord), 0).rgb;

    float bL = Luma(b), dL = Luma(d), eL = Luma(e), fL = Luma(f), hL = Luma(h);
    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    float maxL = max(max(max(bL, dL), max(fL, hL)), eL);
    float minL = min(min(min(bL, dL), min(fL, hL)), eL);
    nz = clamp(abs(nz) / max(maxL - minL, 1e-5), 0.0, 1.0);
    nz = -0.5 * nz + 1.0;

    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));

    vec3 hitMin = mn4 / max(4.0 * mx4, vec3(1e-5));
    vec3 hitMax = (vec3(1.0) - mx4) / min(4.0 * mn4 - vec3(4.0), vec3(-1e-5));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0));

    float sharpScale = exp2(-clamp(uSharpness, 0.0, 2.0));
    lobe *= sharpScale * nz;

    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    vec3 outColor = (lobe * (b + d + f + h) + e) * rcpL;
    FragColor = vec4(clamp(outColor, 0.0, 1.0), 1.0);
}
)GLSL";

GLuint CompileShader(const GlProcs& gl, GLenum type, const char* src) {
    GLuint s = gl.CreateShader(type);
    gl.ShaderSource(s, 1, &src, nullptr);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        gl.GetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::cerr << "[paws] GLSL compile error:\n" << log << "\n";
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

GLuint BuildProgram(const GlProcs& gl, const char* vsSrc, const char* fsSrc) {
    GLuint vs = CompileShader(gl, GL_VERTEX_SHADER, vsSrc);
    GLuint fs = CompileShader(gl, GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) return 0;
    GLuint prog = gl.CreateProgram();
    gl.AttachShader(prog, vs);
    gl.AttachShader(prog, fs);
    gl.LinkProgram(prog);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLint ok = 0;
    gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        gl.GetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::cerr << "[paws] GLSL link error:\n" << log << "\n";
        gl.DeleteProgram(prog);
        return 0;
    }
    return prog;
}

// ============================================================================
// X11 Window Lookup (by PID or Window Title)
// ============================================================================
std::string GetWindowTitle(Display* dpy, Window w) {
    Atom netWmName = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0, bytesAfter = 0;
    unsigned char* prop = nullptr;

    if (XGetWindowProperty(dpy, w, netWmName, 0, 256, False, utf8,
                           &actualType, &actualFormat, &nitems, &bytesAfter, &prop) == Success && prop) {
        std::string title(reinterpret_cast<char*>(prop), nitems);
        XFree(prop);
        return title;
    }

    char* wmName = nullptr;
    if (XFetchName(dpy, w, &wmName) && wmName) {
        std::string title(wmName);
        XFree(wmName);
        return title;
    }
    return {};
}

pid_t GetWindowPid(Display* dpy, Window w) {
    Atom pidAtom = XInternAtom(dpy, "_NET_WM_PID", False);
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0, bytesAfter = 0;
    unsigned char* prop = nullptr;
    pid_t pid = 0;
    if (XGetWindowProperty(dpy, w, pidAtom, 0, 1, False, XA_CARDINAL,
                           &actualType, &actualFormat, &nitems, &bytesAfter, &prop) == Success && prop) {
        if (nitems >= 1) {
            pid = static_cast<pid_t>(*reinterpret_cast<unsigned long*>(prop));
        }
        XFree(prop);
    }
    return pid;
}

Window FindWindowRecursive(Display* dpy, Window root, const std::string& titleLower, pid_t targetPid) {
    Window rootRet = None, parentRet = None;
    Window* children = nullptr;
    unsigned int nchildren = 0;
    if (!XQueryTree(dpy, root, &rootRet, &parentRet, &children, &nchildren)) {
        return None;
    }

    Window found = None;
    for (unsigned int i = 0; i < nchildren && found == None; ++i) {
        Window w = children[i];
        XWindowAttributes attrs{};
        if (XGetWindowAttributes(dpy, w, &attrs) && attrs.map_state == IsViewable &&
            attrs.width >= 160 && attrs.height >= 120) {
            if (targetPid > 0) {
                if (GetWindowPid(dpy, w) == targetPid) {
                    found = w;
                    break;
                }
            } else {
                std::string t = ToLower(GetWindowTitle(dpy, w));
                if (!t.empty() && t.find(titleLower) != std::string::npos) {
                    found = w;
                    break;
                }
            }
        }
        found = FindWindowRecursive(dpy, w, titleLower, targetPid);
    }

    if (children) XFree(children);
    return found;
}

bool IsKeyPressed(Display* dpy, KeySym sym) {
    char keys[32] = {};
    XQueryKeymap(dpy, keys);
    KeyCode kc = XKeysymToKeycode(dpy, sym);
    if (kc == 0) return false;
    return (keys[kc >> 3] & (1 << (kc & 7))) != 0;
}

// ============================================================================
// Live GTA V Auto-Watcher (Linux / Proton / Wine)
// Scans /proc for PlayGTAV.exe / GTAVLauncher.exe / GTA5.exe, automatically
// copies paws_gtav.asi into the GTA V folder when detected, and removes it
// automatically as soon as GTA V exits.
// ============================================================================
fs::path GetSelfDirectoryLinux() {
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !exe.empty()) return exe.parent_path();
    return fs::current_path();
}

fs::path ResolveAsiSourceLinux(const std::string& customAsiPath) {
    if (!customAsiPath.empty() && fs::exists(customAsiPath)) {
        return fs::path(customAsiPath);
    }
    fs::path beside = GetSelfDirectoryLinux() / "paws_gtav.asi";
    if (fs::exists(beside)) return beside;
    fs::path cwdAsi = fs::current_path() / "paws_gtav.asi";
    if (fs::exists(cwdAsi)) return cwdAsi;
    return beside;
}

bool ExtractGtaDirFromArg(const std::string& arg, fs::path& outDir) {
    std::string lower = ToLower(arg);
    if (lower.find("gta5.exe") == std::string::npos &&
        lower.find("playgtav.exe") == std::string::npos &&
        lower.find("gtavlauncher.exe") == std::string::npos) {
        return false;
    }

    std::string pathStr = arg;
    // Convert Wine Z:\path\to\GTA5.exe -> /path/to/GTA5.exe
    if (pathStr.size() >= 3 && (pathStr[0] == 'Z' || pathStr[0] == 'z') && pathStr[1] == ':') {
        pathStr = pathStr.substr(2);
        std::replace(pathStr.begin(), pathStr.end(), '\\', '/');
    }

    if (!pathStr.empty() && pathStr[0] == '/') {
        fs::path p(pathStr);
        std::error_code ec;
        if (fs::exists(p.parent_path(), ec)) {
            outDir = p.parent_path();
            return true;
        }
    }
    return false;
}

bool FindRunningGtaVLinux(pid_t& outPid, fs::path& outGameDir) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/proc", ec)) {
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit)) continue;

        std::ifstream f(entry.path() / "cmdline", std::ios::binary);
        if (!f) continue;
        std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (raw.empty()) continue;

        // Parse NUL-separated arguments
        size_t pos = 0;
        bool matchedGta = false;
        while (pos < raw.size()) {
            size_t end = raw.find('\0', pos);
            if (end == std::string::npos) end = raw.size();
            std::string arg = raw.substr(pos, end - pos);
            pos = end + 1;
            if (arg.empty()) continue;

            std::string lower = ToLower(arg);
            if (lower.find("gta5.exe") != std::string::npos ||
                lower.find("playgtav.exe") != std::string::npos ||
                lower.find("gtavlauncher.exe") != std::string::npos) {
                matchedGta = true;
                fs::path dir;
                if (ExtractGtaDirFromArg(arg, dir)) {
                    outPid = static_cast<pid_t>(std::stol(name));
                    outGameDir = dir;
                    return true;
                }
            }
        }

        if (matchedGta) {
            fs::path cwd = fs::read_symlink(entry.path() / "cwd", ec);
            if (!ec && fs::exists(cwd, ec)) {
                outPid = static_cast<pid_t>(std::stol(name));
                outGameDir = cwd;
                return true;
            }
        }
    }
    return false;
}

void RemoveAsiLinux(const fs::path& dstAsi) {
    if (dstAsi.empty()) return;
    std::error_code ec;
    if (fs::exists(dstAsi, ec)) {
        if (fs::remove(dstAsi, ec) && !ec) {
            std::cout << "[paws] Removed paws_gtav.asi from GTA V folder: " << dstAsi.string() << "\n";
        } else {
            std::cerr << "[paws] WARNING: Failed to remove " << dstAsi.string() << " (" << ec.message() << ")\n";
        }
    }
}

fs::path FindGtaVSettingsXmlLinux() {
    const char* homeEnv = std::getenv("HOME");
    if (!homeEnv) return {};
    fs::path home(homeEnv);

    std::vector<fs::path> steamLibs;
    steamLibs.push_back(home / ".local/share/Steam");

    // Parse Steam libraryfolders.vdf to discover external SteamLibrary drives
    fs::path vdfPath = home / ".local/share/Steam/steamapps/libraryfolders.vdf";
    std::ifstream vdf(vdfPath);
    if (vdf) {
        std::string line;
        std::regex pathRe("\"path\"\\s+\"([^\"]+)\"");
        while (std::getline(vdf, line)) {
            std::smatch m;
            if (std::regex_search(line, m, pathRe)) {
                steamLibs.emplace_back(m[1].str());
            }
        }
    }

    const fs::path relSettings =
        "steamapps/compatdata/271590/pfx/drive_c/users/steamuser/Documents/Rockstar Games/GTA V/settings.xml";
    for (const auto& lib : steamLibs) {
        fs::path candidate = lib / relSettings;
        std::error_code ec;
        if (fs::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

const char* SamplingModeLabel(int mode) {
    switch (mode) {
        case 1: return "1/2 (50% - Performance)";
        case 2: return "2/3 (67% - Quality)";
        case 3: return "3/4 (75% - Ultra Quality)";
        case 4: return "5/6 (83% - Native-Like)";
        default: return "Custom";
    }
}

bool ApplyGtaVSettingsLinux(const fs::path& settingsPath, int targetW, int targetH, int samplingMode) {
    if (settingsPath.empty() || !fs::exists(settingsPath)) return false;

    fs::path backupPath = settingsPath;
    backupPath += ".paws.bak";

    std::error_code ec;
    // Only create backup if one doesn't already exist (preserves true original settings)
    if (!fs::exists(backupPath, ec)) {
        if (!fs::copy_file(settingsPath, backupPath, fs::copy_options::overwrite_existing, ec)) {
            std::cerr << "[paws] WARNING: Could not back up settings.xml (" << ec.message() << ")\n";
            return false;
        }
    }

    std::ifstream in(backupPath, std::ios::binary);
    if (!in) return false;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    std::string patched = content;
    if (targetW > 0 && targetH > 0) {
        patched = std::regex_replace(
            patched,
            std::regex("<ScreenWidth\\s+value=\"[^\"]*\"\\s*/>"),
            "<ScreenWidth value=\"" + std::to_string(targetW) + "\" />");
        patched = std::regex_replace(
            patched,
            std::regex("<ScreenHeight\\s+value=\"[^\"]*\"\\s*/>"),
            "<ScreenHeight value=\"" + std::to_string(targetH) + "\" />");
    }

    if (samplingMode > 0) {
        patched = std::regex_replace(
            patched,
            std::regex("<SamplingMode\\s+value=\"[^\"]*\"\\s*/>"),
            "<SamplingMode value=\"" + std::to_string(samplingMode) + "\" />");
    }

    std::ofstream out(settingsPath, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(patched.data(), static_cast<std::streamsize>(patched.size()));
    out.close();

    std::cout << "[paws] Applied GTA V settings -> Resolution: " << targetW << "x" << targetH;
    if (samplingMode > 0) std::cout << ", Frame Scaling: " << SamplingModeLabel(samplingMode);
    std::cout << "\n       (Original backed up to: " << backupPath.filename().string()
              << " and will auto-restore on exit)\n";
    return true;
}

void RestoreGtaVSettingsLinux(const fs::path& settingsPath) {
    if (settingsPath.empty()) return;
    fs::path backupPath = settingsPath;
    backupPath += ".paws.bak";
    std::error_code ec;
    if (fs::exists(backupPath, ec)) {
        if (fs::copy_file(backupPath, settingsPath, fs::copy_options::overwrite_existing, ec)) {
            fs::remove(backupPath, ec);
            std::cout << "[paws] Restored original GTA V settings.xml cleanly.\n";
        } else {
            std::cerr << "[paws] WARNING: Failed to restore settings.xml (" << ec.message() << ")\n";
        }
    }
}

int RunGtaVLiveWatcherLinux(const std::string& customAsiPath, int samplingMode) {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    fs::path srcAsi = ResolveAsiSourceLinux(customAsiPath);
    fs::path settingsPath = FindGtaVSettingsXmlLinux();

    fs::path srcAnrDir = GetSelfDirectoryLinux() / "ANR_FG";
    std::cout << "[paws] Live GTA V watcher active (Linux / Proton / Wine).\n"
              << "[paws] Source ASI: " << srcAsi.string()
              << (fs::exists(srcAsi) ? " (found)\n" : " (NOT found yet — place Windows-built paws_gtav.asi here or pass --asi <path>)\n")
              << "[paws] ANR FG dir: " << srcAnrDir.string()
              << (fs::exists(srcAnrDir / "nvngx_dlssg.dll") ? " (found - ANR 2X AI Frame Gen ready!)\n" : " (not present - will use FSR RCAS)\n");

    if (!settingsPath.empty()) {
        std::cout << "[paws] Found GTA V settings.xml: " << settingsPath.string() << "\n";
        ApplyGtaVSettingsLinux(settingsPath, 1920, 1080, samplingMode);
    } else {
        std::cout << "[paws] Note: GTA V settings.xml not found yet (is your SteamLibrary drive mounted?).\n";
    }

    std::cout << "[paws] Waiting for GTA V (PlayGTAV.exe / GTAVLauncher.exe / GTA5.exe) to start...\n"
              << "[paws] Press Ctrl+C to stop watching.\n";

    bool active = false;
    fs::path deployedAsi;
    fs::path deployedAnrDir;

    auto removeAnrDir = [](const fs::path& dir) {
        if (dir.empty()) return;
        std::error_code ec;
        if (fs::is_symlink(dir, ec) || fs::exists(dir, ec)) {
            fs::remove_all(dir, ec);
            if (!ec) {
                std::cout << "[paws] Removed ANR_FG from GTA V folder: " << dir.string() << "\n";
            }
        }
    };

    while (!g_ShouldQuit) {
        pid_t pid = 0;
        fs::path gameDir;
        bool running = FindRunningGtaVLinux(pid, gameDir);

        if (running && !active) {
            std::cout << "\n[paws] Detected GTA V starting (PID " << pid << ") in: " << gameDir.string() << "\n";

            if (settingsPath.empty()) settingsPath = FindGtaVSettingsXmlLinux();
            if (!settingsPath.empty() && !fs::exists(settingsPath.string() + ".paws.bak")) {
                ApplyGtaVSettingsLinux(settingsPath, 1920, 1080, samplingMode);
            }

            // Live-deploy ANR_FG directory (symlink for instant zero-copy deploy)
            if (fs::exists(srcAnrDir / "nvngx_dlssg.dll")) {
                fs::path dstAnr = gameDir / "ANR_FG";
                std::error_code ec;
                if (!fs::exists(dstAnr, ec) && !fs::is_symlink(dstAnr, ec)) {
                    fs::create_directory_symlink(srcAnrDir, dstAnr, ec);
                    if (!ec) {
                        std::cout << "[paws] Live-deployed ANR_FG -> " << dstAnr.string() << "\n";
                        deployedAnrDir = dstAnr;
                    }
                }
            }

            fs::path dstAsi = gameDir / "paws_gtav.asi";
            fs::path src = ResolveAsiSourceLinux(customAsiPath);

            if (fs::exists(dstAsi)) {
                std::cout << "[paws] paws_gtav.asi is already present in GTA V folder.\n";
                deployedAsi = dstAsi;
            } else if (fs::exists(src)) {
                std::error_code ec;
                if (fs::copy_file(src, dstAsi, fs::copy_options::overwrite_existing, ec)) {
                    std::cout << "[paws] Live-deployed paws_gtav.asi -> " << dstAsi.string() << "\n";
                    deployedAsi = dstAsi;
                } else {
                    std::cerr << "[paws] ERROR: Failed to copy paws_gtav.asi (" << ec.message() << ").\n";
                }
            } else {
                std::cerr << "[paws] WARNING: paws_gtav.asi not found at " << src.string() << ".\n";
            }

            if (!fs::exists(gameDir / "dinput8.dll")) {
                std::cout << "[paws] WARNING: dinput8.dll not found in " << gameDir.string() << ".\n"
                          << "       Install an ASI loader (dinput8.dll) and set Steam Launch Options to:\n"
                          << "         WINEDLLOVERRIDES=\"dinput8=n,b\" %command%\n";
            } else {
                std::cout << "[paws] Verified dinput8.dll in GTA V folder.\n";
            }
            std::cout << "[paws] Monitoring GTA V — will auto-remove paws_gtav.asi & ANR_FG when GTA V closes.\n";
            active = true;
        } else if (!running && active) {
            std::cout << "\n[paws] Detected GTA V closed.\n";
            RemoveAsiLinux(deployedAsi);
            removeAnrDir(deployedAnrDir);
            RestoreGtaVSettingsLinux(settingsPath);
            deployedAsi.clear();
            deployedAnrDir.clear();
            active = false;
            std::cout << "[paws] Resuming watch for GTA V...\n";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (active) {
        pid_t pid = 0;
        fs::path dir;
        if (!FindRunningGtaVLinux(pid, dir)) {
            RemoveAsiLinux(deployedAsi);
            removeAnrDir(deployedAnrDir);
        }
    }
    RestoreGtaVSettingsLinux(settingsPath);
    std::cout << "\n[paws] Watcher stopped.\n";
    return 0;
}

void PrintUsage() {
    std::cout << "paws - Universal FSR 1.0 Spatial Upscaler & Live GTA V ANR FG Watcher (Linux)\n"
              << "Usage:\n"
              << "  paws [--scale <0|1|2|3>]                      (live-watch GTA V: deploys ANR FG + ASI, removes all on exit)\n"
              << "  paws --watch-gtav [--scale <0|1|2|3>] [--asi <path/paws_gtav.asi>]\n"
              << "  paws <path-to-executable-or-window-title> [--sharpness <0.0-2.0>]\n\n"
              << "Scale Presets (--scale):\n"
              << "  0 = Native (keep your settings.xml, default for ANR 2X Frame Gen)\n"
              << "  1 = 1/2 (50% - Performance)\n"
              << "  2 = 2/3 (67% - Quality)\n"
              << "  3 = 3/4 (75% - Ultra Quality)\n\n"
              << "Hotkeys (FSR 1.0 Overlay):\n"
              << "  F8   Toggle RCAS sharpening on/off\n"
              << "  End  Quit overlay\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return RunGtaVLiveWatcherLinux("", 0);
    }

    std::string targetArg;
    std::string customAsiPath;
    float sharpness = 0.2f;
    int samplingMode = 0; // Default: keep native resolution for ANR 2X Frame Gen
    bool watchGtaV = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            PrintUsage();
            return 0;
        } else if (a == "--watch-gtav" || a == "--watch") {
            watchGtaV = true;
        } else if (a == "--asi" && i + 1 < argc) {
            customAsiPath = argv[++i];
        } else if (a == "--scale" && i + 1 < argc) {
            samplingMode = std::clamp(std::stoi(argv[++i]), 0, 4);
            watchGtaV = true;
        } else if (a == "--sharpness" && i + 1 < argc) {
            sharpness = std::stof(argv[++i]);
        } else if (targetArg.empty()) {
            targetArg = a;
        }
    }

    if (watchGtaV || ToLower(fs::path(targetArg).filename().string()) == "gta5.exe") {
        return RunGtaVLiveWatcherLinux(customAsiPath, samplingMode);
    }

    if (targetArg.empty()) {
        PrintUsage();
        return 1;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::cerr << "[paws] ERROR: Could not open X11 display (is DISPLAY set?).\n";
        return 1;
    }
    XSetErrorHandler(HandleX11Error);

    int compEv = 0, compErr = 0;
    if (!XCompositeQueryExtension(dpy, &compEv, &compErr)) {
        std::cerr << "[paws] ERROR: XComposite extension not available.\n";
        XCloseDisplay(dpy);
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    // Launch executable if path exists and is executable, otherwise attach to window title
    Window targetWin = None;
    pid_t launchedPid = 0;

    if (access(targetArg.c_str(), X_OK) == 0 && fs::is_regular_file(targetArg)) {
        std::cout << "[paws] Launching: " << targetArg << "\n";
        launchedPid = fork();
        if (launchedPid == 0) {
            char* const args[] = { const_cast<char*>(targetArg.c_str()), nullptr };
            execvp(targetArg.c_str(), args);
            _exit(127);
        }
        std::cout << "[paws] Waiting for application window (PID " << launchedPid << ")...\n";
        for (int i = 0; i < 150 && !g_ShouldQuit; ++i) {
            targetWin = FindWindowRecursive(dpy, root, "", launchedPid);
            if (targetWin != None) break;
            int status = 0;
            if (waitpid(launchedPid, &status, WNOHANG) == launchedPid) {
                std::cerr << "[paws] Target process exited before opening a window.\n";
                XCloseDisplay(dpy);
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    } else {
        targetWin = FindWindowRecursive(dpy, root, ToLower(targetArg), 0);
    }

    if (targetWin == None) {
        std::cerr << "[paws] ERROR: Could not find window matching \"" << targetArg << "\".\n";
        XCloseDisplay(dpy);
        return 1;
    }

    std::cout << "[paws] Attached to X11 window 0x" << std::hex << targetWin << std::dec
              << " (\"" << GetWindowTitle(dpy, targetWin) << "\")\n";

    // Redirect target window to offscreen storage via XComposite
    XCompositeRedirectWindow(dpy, targetWin, CompositeRedirectAutomatic);
    XSync(dpy, False);

    // Choose GLX FBConfig
    int fbAttribs[] = {
        GLX_X_RENDERABLE, True,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        GLX_DOUBLEBUFFER, True,
        None
    };

    int fbCount = 0;
    GLXFBConfig* fbcList = glXChooseFBConfig(dpy, screen, fbAttribs, &fbCount);
    if (!fbcList || fbCount == 0) {
        std::cerr << "[paws] ERROR: Failed to choose GLXFBConfig.\n";
        XCloseDisplay(dpy);
        return 1;
    }
    GLXFBConfig fbc = fbcList[0];
    XFree(fbcList);

    XVisualInfo* vi = glXGetVisualFromFBConfig(dpy, fbc);
    if (!vi) {
        std::cerr << "[paws] ERROR: glXGetVisualFromFBConfig failed.\n";
        XCloseDisplay(dpy);
        return 1;
    }

    int screenW = DisplayWidth(dpy, screen);
    int screenH = DisplayHeight(dpy, screen);

    XSetWindowAttributes swa{};
    swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
    swa.override_redirect = True;
    swa.border_pixel = 0;
    swa.background_pixel = 0;

    Window overlayWin = XCreateWindow(
        dpy, root, 0, 0, static_cast<unsigned int>(screenW), static_cast<unsigned int>(screenH),
        0, vi->depth, InputOutput, vi->visual,
        CWColormap | CWOverrideRedirect | CWBorderPixel | CWBackPixel, &swa);
    XFree(vi);

    // Make overlay click-through using XFixes
    XserverRegion emptyRegion = XFixesCreateRegion(dpy, nullptr, 0);
    XFixesSetWindowShapeRegion(dpy, overlayWin, ShapeInput, 0, 0, emptyRegion);
    XFixesDestroyRegion(dpy, emptyRegion);

    XMapRaised(dpy, overlayWin);
    XSync(dpy, False);

    // Create OpenGL 3.3 Core context
    using PFNGLXCREATECONTEXTATTRIBSARBPROC =
        GLXContext (*)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
    auto glXCreateContextAttribsARB = reinterpret_cast<PFNGLXCREATECONTEXTATTRIBSARBPROC>(
        glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXCreateContextAttribsARB")));

    GLXContext glCtx = nullptr;
    if (glXCreateContextAttribsARB) {
        int ctxAttribs[] = {
            GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
            GLX_CONTEXT_MINOR_VERSION_ARB, 3,
            GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
            None
        };
        glCtx = glXCreateContextAttribsARB(dpy, fbc, nullptr, True, ctxAttribs);
    }
    if (!glCtx) {
        glCtx = glXCreateNewContext(dpy, fbc, GLX_RGBA_TYPE, nullptr, True);
    }
    if (!glCtx || !glXMakeCurrent(dpy, overlayWin, glCtx)) {
        std::cerr << "[paws] ERROR: Failed to create/bind GLX context.\n";
        XDestroyWindow(dpy, overlayWin);
        XCloseDisplay(dpy);
        return 1;
    }

    GlProcs gl{};
    if (!gl.Load()) {
        std::cerr << "[paws] ERROR: Missing required OpenGL 3.3 functions.\n";
        glXMakeCurrent(dpy, None, nullptr);
        glXDestroyContext(dpy, glCtx);
        XDestroyWindow(dpy, overlayWin);
        XCloseDisplay(dpy);
        return 1;
    }

    GLuint easuProg = BuildProgram(gl, kFullscreenVS, kFsrEasuFS);
    GLuint rcasProg = BuildProgram(gl, kFullscreenNoFlipVS, kFsrRcasFS);
    if (!easuProg || !rcasProg) {
        glXMakeCurrent(dpy, None, nullptr);
        glXDestroyContext(dpy, glCtx);
        XDestroyWindow(dpy, overlayWin);
        XCloseDisplay(dpy);
        return 1;
    }

    GLuint vao = 0;
    gl.GenVertexArrays(1, &vao);
    gl.BindVertexArray(vao);

    // Source texture (updated from XComposite pixmap)
    GLuint srcTex = 0;
    glGenTextures(1, &srcTex);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Intermediate EASU framebuffer texture (screen resolution)
    GLuint easuTex = 0, easuFbo = 0;
    glGenTextures(1, &easuTex);
    glBindTexture(GL_TEXTURE_2D, easuTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, screenW, screenH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    gl.GenFramebuffers(1, &easuFbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, easuFbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, easuTex, 0);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);

    GLint locEasuSrc = gl.GetUniformLocation(easuProg, "uSrcTex");
    GLint locEasuSize = gl.GetUniformLocation(easuProg, "uSrcSize");
    GLint locEasuInvSize = gl.GetUniformLocation(easuProg, "uInvSrcSize");
    GLint locRcasTex = gl.GetUniformLocation(rcasProg, "uEasuTex");
    GLint locRcasSharp = gl.GetUniformLocation(rcasProg, "uSharpness");
    GLint locRcasEnable = gl.GetUniformLocation(rcasProg, "uEnableRcas");

    bool enableRcas = true;
    bool prevF8 = false;
    int texW = 0, texH = 0;
    int frameCount = 0;
    auto fpsTimer = std::chrono::steady_clock::now();

    std::cout << "[paws] FSR 1.0 overlay active (" << screenW << "x" << screenH << ").\n";
    std::cout << "[paws] Hotkeys: F8 = Toggle RCAS Sharpening | End = Quit\n";

    while (!g_ShouldQuit) {
        // Hotkeys
        bool f8Down = IsKeyPressed(dpy, XK_F8);
        if (f8Down && !prevF8) {
            enableRcas = !enableRcas;
            std::cout << "\n[paws] RCAS Sharpening: " << (enableRcas ? "ON" : "OFF") << "\n";
        }
        prevF8 = f8Down;

        if (IsKeyPressed(dpy, XK_End)) {
            std::cout << "\n[paws] End pressed. Exiting.\n";
            break;
        }

        // Capture target window via XComposite
        g_X11ErrorOccurred = false;
        XWindowAttributes attrs{};
        if (!XGetWindowAttributes(dpy, targetWin, &attrs) || g_X11ErrorOccurred) {
            std::cout << "\n[paws] Target window closed. Exiting.\n";
            break;
        }

        if (attrs.width >= 16 && attrs.height >= 16) {
            Pixmap pixmap = XCompositeNameWindowPixmap(dpy, targetWin);
            if (!g_X11ErrorOccurred && pixmap != None) {
                XImage* img = XGetImage(dpy, pixmap, 0, 0,
                                        static_cast<unsigned int>(attrs.width),
                                        static_cast<unsigned int>(attrs.height),
                                        AllPlanes, ZPixmap);
                if (img && img->data) {
                    gl.ActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, srcTex);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                    if (texW != img->width || texH != img->height) {
                        texW = img->width;
                        texH = img->height;
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texW, texH, 0,
                                     GL_BGRA, GL_UNSIGNED_BYTE, img->data);
                    } else {
                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texW, texH,
                                        GL_BGRA, GL_UNSIGNED_BYTE, img->data);
                    }
                    XDestroyImage(img);
                }
                XFreePixmap(dpy, pixmap);
            }
        }

        if (texW > 0 && texH > 0) {
            glViewport(0, 0, screenW, screenH);
            gl.BindVertexArray(vao);

            // Pass 1: EASU -> easuFbo
            gl.BindFramebuffer(GL_FRAMEBUFFER, easuFbo);
            gl.UseProgram(easuProg);
            gl.ActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, srcTex);
            gl.Uniform1i(locEasuSrc, 0);
            gl.Uniform2f(locEasuSize, static_cast<float>(texW), static_cast<float>(texH));
            gl.Uniform2f(locEasuInvSize, 1.0f / static_cast<float>(texW), 1.0f / static_cast<float>(texH));
            glDrawArrays(GL_TRIANGLES, 0, 3);

            // Pass 2: RCAS -> default framebuffer
            gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
            gl.UseProgram(rcasProg);
            gl.ActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, easuTex);
            gl.Uniform1i(locRcasTex, 0);
            gl.Uniform1f(locRcasSharp, sharpness);
            gl.Uniform1i(locRcasEnable, enableRcas ? 1 : 0);
            glDrawArrays(GL_TRIANGLES, 0, 3);

            glXSwapBuffers(dpy, overlayWin);

            ++frameCount;
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - fpsTimer).count();
            if (dt >= 0.5) {
                double fps = static_cast<double>(frameCount) / dt;
                std::printf("\r[paws] FPS: %6.1f | %dx%d -> %dx%d | RCAS: %-3s   ",
                            fps, texW, texH, screenW, screenH, enableRcas ? "ON" : "OFF");
                std::fflush(stdout);
                frameCount = 0;
                fpsTimer = now;
            }
        }
    }

    gl.DeleteFramebuffers(1, &easuFbo);
    glDeleteTextures(1, &easuTex);
    glDeleteTextures(1, &srcTex);
    gl.DeleteVertexArrays(1, &vao);
    gl.DeleteProgram(easuProg);
    gl.DeleteProgram(rcasProg);

    glXMakeCurrent(dpy, None, nullptr);
    glXDestroyContext(dpy, glCtx);
    XDestroyWindow(dpy, overlayWin);
    XCompositeUnredirectWindow(dpy, targetWin, CompositeRedirectAutomatic);
    XCloseDisplay(dpy);
    std::cout << "\n[paws] Shutdown complete.\n";
    return 0;
}
