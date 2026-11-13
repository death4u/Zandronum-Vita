// [Vita] OpenGL through vitaGL.
#ifndef VITA_GL_GL_H
#define VITA_GL_GL_H
#include <vitaGL.h>

// Sized formats vitaGL doesn't name; the renderer only uses them as hints.
#ifndef GL_ALPHA8
#define GL_ALPHA8 GL_ALPHA
#endif
#ifndef GL_RGBA2
#define GL_RGBA2 GL_RGBA
#endif

// Fixed-function bits vitaGL leaves out. Enabling them only raises
// GL_INVALID_ENUM, which the renderer never checks, and the functions below
// are no-ops: anti-aliased lines, dithering, quad-buffered stereo and
// sphere-map texture generation (a rarely used material effect).
#ifndef GL_LINE_SMOOTH
#define GL_LINE_SMOOTH          0x0B20
#endif
#ifndef GL_POLYGON_SMOOTH
#define GL_POLYGON_SMOOTH       0x0B41
#endif
#ifndef GL_LINE_SMOOTH_HINT
#define GL_LINE_SMOOTH_HINT     0x0C52
#endif
#ifndef GL_POLYGON_SMOOTH_HINT
#define GL_POLYGON_SMOOTH_HINT  0x0C53
#endif
#ifndef GL_FOG_HINT
#define GL_FOG_HINT             0x0C54
#endif
#ifndef GL_DITHER
#define GL_DITHER               0x0BD0
#endif
#ifndef GL_STEREO
#define GL_STEREO               0x0C33
#endif
#ifndef GL_DRAW_BUFFER
#define GL_DRAW_BUFFER          0x0C01
#endif
#ifndef GL_BACK_LEFT
#define GL_BACK_LEFT            0x0402
#endif
#ifndef GL_BACK_RIGHT
#define GL_BACK_RIGHT           0x0403
#endif
#ifndef GL_INDEX_ARRAY
#define GL_INDEX_ARRAY          0x8077
#endif
#ifndef GL_S
#define GL_S                    0x2000
#define GL_T                    0x2001
#endif
#ifndef GL_TEXTURE_GEN_MODE
#define GL_TEXTURE_GEN_MODE     0x2500
#endif
#ifndef GL_SPHERE_MAP
#define GL_SPHERE_MAP           0x2402
#endif
#ifndef GL_TEXTURE_GEN_S
#define GL_TEXTURE_GEN_S        0x0C60
#define GL_TEXTURE_GEN_T        0x0C61
#endif

static inline void glDrawBuffer(GLenum mode) { (void)mode; }
static inline void glTexGeni(GLenum coord, GLenum pname, GLint param) { (void)coord; (void)pname; (void)param; }
static inline void glGetObjectParameteriv(GLuint obj, GLenum pname, GLint *params) { glGetShaderiv(obj, pname, params); }

#endif
