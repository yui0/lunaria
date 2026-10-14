/* Shared C geometry for Android Path and VectorDrawable. */
#ifndef DVM_PATH_H
#define DVM_PATH_H
#include <stddef.h>
#include <stdbool.h>
#include "../luna-ui/nanosvg.h"
typedef struct luna_path {
   NSVGpath *first, *last, *spare;
   size_t capacity;
   int fill;
   bool after_close;
} luna_path;
void luna_path_free(void *path);
void luna_path_clear(luna_path *path,bool reset_fill);
void luna_path_rewind(luna_path *path);
bool luna_path_move(luna_path *path,float x,float y);
bool luna_path_line(luna_path *path,float x,float y);
bool luna_path_cubic(luna_path *path,const float points[6]);
bool luna_path_quad(luna_path *path,float x1,float y1,float x2,float y2);
bool luna_path_close(luna_path *path);
bool luna_path_append(luna_path *path,const luna_path *other,const float *matrix);
bool luna_path_transform(luna_path *path,const float matrix[9]);
bool luna_path_arc(luna_path *path,const float rect[4],float start,float sweep,bool move);
bool luna_path_parse(luna_path *path,const char *data);
bool luna_path_trim(luna_path *out,const luna_path *path,float start,float end,float offset);
void luna_path_bounds(const luna_path *path,float bounds[4]);
/* Fraction/x/y triples, owned by caller. Moves retain the same fraction. */
float *luna_path_approximate(const luna_path *path,float error,size_t *count);
/* RGBA mask, with top-left origin; free() by caller. Matrix is row-major. */
unsigned char *luna_path_mask(const luna_path *path,int w,int h,const float matrix[9],
                             int style,float stroke,int cap,int join,float miter,bool aa);
#endif
