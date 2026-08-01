// texconv_wrap.c - Thin C wrapper around Texconv-Custom-DLL shared library.
// Uses LoadLibrary on Windows, dlopen elsewhere.

#if _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 0x00000001;
#else
#include <dlfcn.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef _WIN32
static HMODULE tc_handle = NULL;
#else
static void *tc_handle = NULL;
#endif

/* texconv signature from the library:
   int texconv(int argc, wchar_t* argv[], bool verbose,
               bool init_com, bool allow_slow_codec,
               wchar_t* err_buf, int err_buf_size) */
typedef int (*texconv_func)(int argc, wchar_t *argv[],
                            int verbose, int init_com, int allow_slow_codec,
                            wchar_t *err_buf, int err_buf_size);
typedef int (*texconv_version_func)(void);

static texconv_func tc_fn = NULL;
static texconv_version_func tc_version_fn = NULL;

/* Allocate and convert a narrow string to wide char. Returns malloc'd buffer. */
static wchar_t *narrow_to_wide(const char *s) {
    if (!s) {
        wchar_t *w = malloc(sizeof(wchar_t));
        if (w) *w = L'\0';
        return w;
    }

#if _WIN32
    /* On Windows use MultiByteToWideChar with UTF-8 */
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (len <= 0) return NULL;
    wchar_t *w = malloc(len * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, len);
    return w;
#else
    size_t slen = strlen(s);
    wchar_t *w = malloc((slen + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    size_t n = mbstowcs(w, s, slen + 1);
    if (n == (size_t)-1) {
        /* Fallback: copy byte-by-byte */
        for (size_t i = 0; i <= slen; i++) w[i] = (wchar_t)(unsigned char)s[i];
    }
    return w;
#endif
}

/* Wide string to narrow (for error message copy back). */
static void wide_to_narrow(char *buf, size_t bufsize, const wchar_t *ws) {
    if (!buf || !ws || bufsize == 0) return;

#if _WIN32
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
    if (n <= 0 || (size_t)n > bufsize) {
        buf[0] = '\0';
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, buf, (int)bufsize, NULL, NULL);
#else
    size_t n = wcstombs(buf, ws, bufsize - 1);
    if (n == (size_t)-1 || n >= bufsize) {
        buf[0] = '\0';
        return;
    }
#endif
}

int load_texconv_lib(const char *lib_path) {
    if (tc_handle) {
#if _WIN32
        FreeLibrary(tc_handle);
#else
        dlclose(tc_handle);
#endif
        tc_handle = NULL;
        tc_fn = NULL;
        tc_version_fn = NULL;
    }

#if _WIN32
    /* Convert path to wide for LoadLibraryW */
    wchar_t *wpath = narrow_to_wide(lib_path);
    if (!wpath) return -1;
    tc_handle = LoadLibraryW(wpath);
    free(wpath);
    if (!tc_handle) return -1;
    tc_fn = (texconv_func)GetProcAddress(tc_handle, "texconv");
    tc_version_fn = (texconv_version_func)GetProcAddress(tc_handle, "texconv_version");
#else
    tc_handle = dlopen(lib_path, RTLD_NOW);
    if (!tc_handle) return -1;
    tc_fn = (texconv_func)dlsym(tc_handle, "texconv");
    tc_version_fn = (texconv_version_func)dlsym(tc_handle, "texconv_version");
#endif

    if (!tc_fn) {
#if _WIN32
        FreeLibrary(tc_handle);
#else
        dlclose(tc_handle);
#endif
        tc_handle = NULL;
        tc_version_fn = NULL;
        return -2;
    }
    return 0;
}

void unload_texconv_lib(void) {
    if (tc_handle) {
#if _WIN32
        FreeLibrary(tc_handle);
#else
        dlclose(tc_handle);
#endif
        tc_handle = NULL;
    }
    tc_fn = NULL;
    tc_version_fn = NULL;
}

int get_texconv_version(void) {
    return tc_version_fn ? tc_version_fn() : 0;
}

/* Call texconv with narrow C strings. Handles wchar_t conversion internally. */
int run_texconv(int argc, const char **argv,
                int verbose, int init_com, int allow_slow,
                char *err_buf, int buf_size) {
    if (!tc_fn) return -99; /* not loaded */

    wchar_t *wargs[128];
    if (argc > 128) return -98;

    for (int i = 0; i < argc; i++) {
        wargs[i] = narrow_to_wide(argv[i]);
        if (!wargs[i]) {
            for (int j = 0; j < i; j++) free(wargs[j]);
            return -97;
        }
    }

    wchar_t werr[256] = {0};

    int ret = tc_fn(argc, wargs, verbose, init_com, allow_slow, werr, 256);

    for (int i = 0; i < argc; i++) free(wargs[i]);

    if (ret != 0 && buf_size > 0) {
        wide_to_narrow(err_buf, (size_t)buf_size, werr);
        if (err_buf[0] == '\0') {
            snprintf(err_buf, (size_t)buf_size, "texconv returned error %d", ret);
        }
    } else if (buf_size > 0) {
        err_buf[0] = '\0';
    }

    return ret;
}
