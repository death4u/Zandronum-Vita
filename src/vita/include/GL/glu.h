// [Vita] The few GLU entry points the GL renderer uses (vita/vita_glu.cpp).
#ifndef VITA_GL_GLU_H
#define VITA_GL_GLU_H
#include <GL/gl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GLUtesselator GLUtesselator;
typedef void (*_GLUfuncptr)(void);

#define GLU_TESS_BEGIN          100100
#define GLU_TESS_VERTEX         100101
#define GLU_TESS_END            100102
#define GLU_TESS_ERROR          100103
#define GLU_TESS_EDGE_FLAG      100104
#define GLU_TESS_COMBINE        100105
#define GLU_TESS_BEGIN_DATA     100106
#define GLU_TESS_VERTEX_DATA    100107
#define GLU_TESS_END_DATA       100108
#define GLU_TESS_ERROR_DATA     100109
#define GLU_TESS_EDGE_FLAG_DATA 100110
#define GLU_TESS_COMBINE_DATA   100111

GLUtesselator *gluNewTess(void);
void gluDeleteTess(GLUtesselator *tess);
void gluTessCallback(GLUtesselator *tess, GLenum which, _GLUfuncptr fn);
void gluTessBeginPolygon(GLUtesselator *tess, GLvoid *data);
void gluTessBeginContour(GLUtesselator *tess);
void gluTessVertex(GLUtesselator *tess, GLdouble *location, GLvoid *data);
void gluTessEndContour(GLUtesselator *tess);
void gluTessEndPolygon(GLUtesselator *tess);

#ifdef __cplusplus
}
#endif
#endif
