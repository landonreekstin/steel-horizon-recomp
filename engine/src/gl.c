#include "gl.h"

#include <stdio.h>

#define DEFINE_GL_FN(ret, name, params) name##_fn p_##name;
GL_FUNCS(DEFINE_GL_FN)
#undef DEFINE_GL_FN

int gl_load(void)
{
#define LOAD_GL_FN(ret, name, params) \
    p_##name = (name##_fn)SDL_GL_GetProcAddress(#name); \
    if (!p_##name) { fprintf(stderr, "missing GL function %s\n", #name); return 0; }
    GL_FUNCS(LOAD_GL_FN)
#undef LOAD_GL_FN
    return 1;
}
