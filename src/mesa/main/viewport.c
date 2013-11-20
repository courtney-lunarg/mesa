/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2009  VMware, Inc.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


/**
 * \file viewport.c
 * glViewport and glDepthRange functions.
 */


#include "context.h"
#include "macros.h"
#include "mtypes.h"
#include "viewport.h"

struct gl_viewport_inputs
{
   GLfloat X, Y;                /**< position */
   GLfloat Width, Height;	/**< size */
};

struct gl_depthrange_inputs
{
   GLdouble Near, Far;		/**< Depth buffer range */
};

/**
 * Set the viewport.
 * \sa Called via glViewport() or display list execution.
 *
 * Flushes the vertices and calls _mesa_set_viewport() with the given
 * parameters.
 */
void GLAPIENTRY
_mesa_Viewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
   GLuint i;
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_VERTICES(ctx, 0);

   if (MESA_VERBOSE & VERBOSE_API)
      _mesa_debug(ctx, "glViewport %d %d %d %d\n", x, y, width, height);

   if (width < 0 || height < 0) {
      _mesa_error(ctx,  GL_INVALID_VALUE,
                   "glViewport(%d, %d, %d, %d)", x, y, width, height);
      return;
   }

   /* ARB_viewport_array specifies that glScissor is equivalent to
    * calling glViewportArray with an array containing a single
    * viewport once for each supported viewport.
    */
   for (i = 0; i < ctx->Const.MaxViewports; i++) {
      _mesa_set_viewporti(ctx, i, x, y, width, height);
   }
}

/**
 * Set new viewport parameters and update derived state (the _WindowMap
 * matrix).  Usually called from _mesa_Viewport().
 *
 * \param ctx GL context.
 * \param x, y coordinates of the lower left corner of the viewport rectangle.
 * \param width width of the viewport rectangle.
 * \param height height of the viewport rectangle.
 */
void
_mesa_ViewportArrayv(GLuint first, GLsizei count, const GLfloat *v)
{
   GLuint i;
   struct gl_viewport_inputs *p = (struct gl_viewport_inputs *) v;
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_API)
      _mesa_debug(ctx, "glViewportArrayv %d %d\n", first, count);

   if ((first + count) > ctx->Const.MaxViewports) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glViewportArrayv: first (%d) + count (%d) > MaxViewports (%d)",
                  first, count, ctx->Const.MaxViewports);
      return;
   }

   /* Verify width & height */
   for (i = 0; i < count; i++) {
      if (p[i].Width < 0 || p[i].Height < 0) {
         _mesa_error(ctx, GL_INVALID_VALUE,
                     "glViewportArrayv: index (%d) width or height < 0 (%f, %f)",
                     i + first, p[i].Width, p[i].Height);
      }
   }

   for (i = 0; i < count; i++)
      _mesa_set_viewporti(ctx, i + first, p[i].X, p[i].Y, p[i].Width, p[i].Height);
}

void GLAPIENTRY
_mesa_ViewportIndexedf(GLuint index, GLfloat x, GLfloat y,
                       GLfloat w, GLfloat h)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_API)
      _mesa_debug(ctx, "glViewportIndexedf(%d, %f, %f, %f, %f)\n",
                  index, x, y, w, h);

   if (index >= ctx->Const.MaxViewports) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glViewportIndexedf: index (%d) >= MaxViewports (%d)",
                  index, ctx->Const.MaxViewports);
      return;
   }

   /* Verify width & height */
   if (w < 0 || h < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glViewportArrayv: index (%d) width or height < 0 (%f, %f)",
                  index, w, h);
   }

   _mesa_set_viewporti(ctx, index, x, y, w, h);
}

void GLAPIENTRY
_mesa_ViewportIndexedfv(GLuint index, const GLfloat * v)
{
   struct gl_viewport_inputs *p = (struct gl_viewport_inputs *) v;
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_API)
      _mesa_debug(ctx, "glViewportIndexedv(%d, %f, %f, %f, %f)\n",
                  index, p->X, p->Y, p->Width, p->Height);

   if (index >= ctx->Const.MaxViewports) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glViewportIndexedf: index (%d) >= MaxViewports (%d)",
                  index, ctx->Const.MaxViewports);
      return;
   }

   /* Verify width & height */
   if (p->Width < 0 || p->Height < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glViewportArrayv: index (%d) width or height < 0 (%f, %f)",
                  index, p->Width, p->Height);
   }

   _mesa_set_viewporti(ctx, index, p->X, p->Y, p->Width, p->Height);
}

/**
 * Set new viewport parameters and update derived state (the _WindowMap
 * matrix).  Usually called from _mesa_Viewport().
 *
 * \param ctx GL context.
 * \param index  index of viewport to update
 * \param x, y   coordinates of the lower left corner of the viewport rectangle.
 * \param width  width of the viewport rectangle.
 * \param height height of the viewport rectangle.
 */
void
_mesa_set_viewporti(struct gl_context *ctx, GLuint index,
                    GLfloat x, GLfloat y,
                    GLfloat width, GLfloat height)
{
   /* clamp width and height to the implementation dependent range */
   width = MIN2(width, (GLsizei) ctx->Const.MaxViewportWidth);
   height = MIN2(height, (GLsizei) ctx->Const.MaxViewportHeight);
   
   /* The location of the viewport's bottom-left corner, given by (x,y), are
    * clamped to be within the implementation-dependent viewport bounds range.
    */
   x = CLAMP(x, (GLint)ctx->Const.ViewportBounds.Min, (GLint)ctx->Const.ViewportBounds.Max);
   y = CLAMP(y, (GLint)ctx->Const.ViewportBounds.Min, (GLint)ctx->Const.ViewportBounds.Max);

   ctx->ViewportArray[index].X = (GLfloat) x;
   ctx->ViewportArray[index].Width = (GLfloat) width;
   ctx->ViewportArray[index].Y = (GLfloat) y;
   ctx->ViewportArray[index].Height = (GLfloat) height;

   ctx->NewState |= _NEW_VIEWPORT;

#if 1
   /* XXX remove this someday.  Currently the DRI drivers rely on
    * the WindowMap matrix being up to date in the driver's Viewport
    * and DepthRange functions.
    */
   _math_matrix_viewport(&ctx->ViewportArray[index]._WindowMap,
                         ctx->ViewportArray[index].X, ctx->ViewportArray[index].Y,
                         ctx->ViewportArray[index].Width, ctx->ViewportArray[index].Height,
                         ctx->ViewportArray[index].Near, ctx->ViewportArray[index].Far,
                         ctx->DrawBuffer->_DepthMaxF);
#endif

   if (ctx->Driver.Viewport) {
      /* Many drivers will use this call to check for window size changes
       * and reallocate the z/stencil/accum/etc buffers if needed.
       */
      ctx->Driver.Viewport(ctx, index);
   }
}

/**
 * Part of ARB_viewport_array extension
 * Update a single DepthRange
 *
 * \param index    array index to update
 * \param nearval  specifies the Z buffer value which should correspond to
 *                 the near clip plane
 * \param farval   specifies the Z buffer value which should correspond to
 *                 the far clip plane
 */
void
_mesa_set_depthrangei(struct gl_context *ctx, GLuint index,
                      GLclampd nearval, GLclampd farval)
{
   FLUSH_VERTICES(ctx, 0);

   ctx->ViewportArray[index].Near = (GLdouble) CLAMP(nearval, 0.0, 1.0);
   ctx->ViewportArray[index].Far = (GLdouble) CLAMP(farval, 0.0, 1.0);

   ctx->NewState |= _NEW_VIEWPORT;

#if 1
   /* XXX remove this someday.  Currently the DRI drivers rely on
    * the WindowMap matrix being up to date in the driver's Viewport
    * and DepthRange functions.
    */
   _math_matrix_viewport(&ctx->ViewportArray[index]._WindowMap,
                         ctx->ViewportArray[index].X, ctx->ViewportArray[index].Y,
                         ctx->ViewportArray[index].Width, ctx->ViewportArray[index].Height,
                         ctx->ViewportArray[index].Near, ctx->ViewportArray[index].Far,
                         ctx->DrawBuffer->_DepthMaxF);
#endif

   if (ctx->Driver.DepthRange) {
      ctx->Driver.DepthRange(ctx, index, nearval, farval);
   }
}

/**
 * Called by glDepthRange
 *
 * \param nearval  specifies the Z buffer value which should correspond to
 *                 the near clip plane
 * \param farval  specifies the Z buffer value which should correspond to
 *                the far clip plane
 */
void GLAPIENTRY
_mesa_DepthRange(GLclampd nearval, GLclampd farval)
{
   GLint i;
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_API)
      _mesa_debug(ctx, "glDepthRange %f %f\n", nearval, farval);

   if (ctx->ViewportArray[0].Near == nearval &&
       ctx->ViewportArray[0].Far == farval)
      return;

   /* glDepthRange is equivalent to calling glViewportArray with an
    * array containing a single viewport once for each supported
    * viewport. It therefore defines all viewports in a single call.
    */
   for (i = 0; i < ctx->Const.MaxViewports; i++) {
      _mesa_set_depthrangei(ctx, i, nearval, farval);
   }
}

void GLAPIENTRY
_mesa_DepthRangef(GLclampf nearval, GLclampf farval)
{
   _mesa_DepthRange(nearval, farval);
}

/**
 * Part of ARB_viewport_array extension
 * Update a range DepthRange values
 *
 * \param first   starting array index
 * \param count   count of DepthRange items to update
 * \param v       pointer to memory containing
 *                GLclampd near and far clip-plane values
 */
void GLAPIENTRY
_mesa_DepthRangeArrayv(GLuint first, GLsizei count, const GLclampd * v)
{
   GLuint i;
   struct gl_depthrange_inputs *p = (struct gl_depthrange_inputs *) v;
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_API)
      _mesa_debug(ctx, "glDepthRangeArrayv %d %d\n", first, count);

   if ((first + count) > ctx->Const.MaxViewports) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glDepthRangev: first (%d) + count (%d) >= MaxViewports (%d)",
                  first, count, ctx->Const.MaxViewports);
      return;
   }

   for (i = 0; i < count; i++) {
      _mesa_set_depthrangei(ctx, i + first, p[i].Near, p[i].Far);
   }
}

/**
 * Part of ARB_viewport_array extension
 * Update a single DepthRange
 *
 * \param index    array index to update
 * \param nearval  specifies the Z buffer value which should correspond to
 *                 the near clip plane
 * \param farval   specifies the Z buffer value which should correspond to
 *                 the far clip plane
 */
void GLAPIENTRY
_mesa_DepthRangeIndexed(GLuint index, GLclampd nearval, GLclampd farval)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_API)
      _mesa_debug(ctx, "glDepthRangeIndexed(%d, %f, %f)\n", index, nearval, farval);

   if (index >= ctx->Const.MaxViewports) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glDepthRangeIndexed: index (%d) >= MaxViewports (%d)",
                  index, ctx->Const.MaxViewports);
      return;
   }

   _mesa_set_depthrangei(ctx, index, nearval, farval);
}

/** 
 * Initialize the context viewport attribute group.
 * \param ctx  the GL context.
 */
void _mesa_init_viewport(struct gl_context *ctx)
{
   GLint i;
   GLfloat depthMax = 65535.0F; /* sorf of arbitrary */

   /* Viewport group */

   for (i = 0; i < MAX_VIEWPORTS; i++) {
      ctx->ViewportArray[i].X = 0.0F;
      ctx->ViewportArray[i].Width = 0.0F;
      ctx->ViewportArray[i].Y = 0.0F;
      ctx->ViewportArray[i].Height = 0.0F;
      ctx->ViewportArray[i].Near = 0.0F;
      ctx->ViewportArray[i].Far = 1.0F;
      _math_matrix_ctr(&ctx->ViewportArray[i]._WindowMap);

      _math_matrix_viewport(&ctx->ViewportArray[i]._WindowMap, 0, 0, 0, 0,
                            0.0F, 1.0F, depthMax);
   }
}

/** 
 * Free the context viewport attribute group data.
 * \param ctx  the GL context.
 */
void _mesa_free_viewport_data(struct gl_context *ctx)
{
   GLint i;

   for (i = 0; i < ctx->Const.MaxViewports; i++) {
      _math_matrix_dtr(&ctx->ViewportArray[i]._WindowMap);
   }

}

