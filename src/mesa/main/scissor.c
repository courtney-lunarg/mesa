/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2007  Brian Paul   All Rights Reserved.
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


#include "main/glheader.h"
#include "main/context.h"
#include "main/mtypes.h"
#include "main/scissor.h"


/**
 * Called via glScissor
 */
void GLAPIENTRY
_mesa_Scissor( GLint x, GLint y, GLsizei width, GLsizei height )
{
   GLuint i;
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_API)
      _mesa_debug(ctx, "glScissor %d %d %d %d\n", x, y, width, height);

   if (width < 0 || height < 0) {
      _mesa_error( ctx, GL_INVALID_VALUE, "glScissor" );
      return;
   }

   /* ARB_viewport_array specifies that glScissor is equivalent to
    * calling glViewportArray with an array containing a single
    * viewport once for each supported viewport.
    */
   for (i = 0; i < ctx->Const.MaxViewports; i++) {
      _mesa_set_scissori(ctx, i, x, y, width, height);
   }
}


/**
 * Define the scissor box.
 *
 * \param x, y coordinates of the scissor box lower-left corner.
 * \param width width of the scissor box.
 * \param height height of the scissor box.
 *
 * \sa glScissor().
 *
 * Verifies the parameters and updates __struct gl_contextRec::Scissor. On a
 * change flushes the vertices and notifies the driver via
 * the dd_function_table::Scissor callback.
 */
void
_mesa_set_scissori(struct gl_context *ctx, GLuint index,
                  GLint x, GLint y, GLsizei width, GLsizei height)
{
   if (x == ctx->Scissor.ScissorArray[index].X &&
       y == ctx->Scissor.ScissorArray[index].Y &&
       width == ctx->Scissor.ScissorArray[index].Width &&
       height == ctx->Scissor.ScissorArray[index].Height)
      return;

   FLUSH_VERTICES(ctx, _NEW_SCISSOR);
   ctx->Scissor.ScissorArray[index].X = x;
   ctx->Scissor.ScissorArray[index].Y = y;
   ctx->Scissor.ScissorArray[index].Width = width;
   ctx->Scissor.ScissorArray[index].Height = height;

   if (ctx->Driver.Scissor)
      ctx->Driver.Scissor(ctx, index,
                          ctx->Scissor.ScissorArray[index].X, ctx->Scissor.ScissorArray[index].Y,
                          ctx->Scissor.ScissorArray[index].Width, ctx->Scissor.ScissorArray[index].Height);
}

/**
 * Define count scissor boxes starting at index.
 *
 * \param index  index of first scissor records to set
 * \param count  number of scissor records to set
 * \param x, y   pointer to array of struct gl_scissor_rects
 *
 * \sa glScissorArrayv().
 *
 * Verifies the parameters and call _mesa_set_scissori to do the work.
 */
void GLAPIENTRY
_mesa_ScissorArrayv(GLuint first, GLsizei count, const GLint * v)
{
   GLuint i;
   struct gl_scissor_rect *p = (struct gl_scissor_rect *) v;
   GET_CURRENT_CONTEXT(ctx);

   if ((first + count) > ctx->Const.MaxViewports) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glScissorArrayv: first (%d) + count (%d) >= MaxViewports (%d)",
                  first, count, ctx->Const.MaxViewports);
      return;
   }

   /* Verify width & height */
   for (i = 0; i < count; i++) {
      if (p[i].Width < 0 || p[i].Height < 0) {
         _mesa_error(ctx, GL_INVALID_VALUE,
                     "glScissorArrayv: index (%d) width or height < 0 (%d, %d)",
                     i, p[i].Width, p[i].Height);
      }
   }

   for (i = 0; i < count; i++)
      _mesa_set_scissori(ctx, i + first, p[i].X, p[i].Y, p[i].Width, p[i].Height);

}

/**
 * Define the scissor box.
 *
 * \param index  index of scissor records to set
 * \param x, y   coordinates of the scissor box lower-left corner.
 * \param width  width of the scissor box.
 * \param height height of the scissor box.
 *
 * \sa glScissorIndexed().
 *
 * Verifies the parameters call _mesa_set_scissori to do the work.
 */
void GLAPIENTRY
_mesa_ScissorIndexed(GLuint index, GLint left, GLint bottom,
                     GLsizei width, GLsizei height)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_API)
      _mesa_debug(ctx, "glScissorIndexed(%d, %d, %d, %d, %d)\n", index,
                  left, bottom, width, height);

   if (width < 0 || height < 0) {
         _mesa_error(ctx, GL_INVALID_VALUE,
                     "glScissorIndexed: index (%d) width or height < 0 (%d, %d)",
                     index, width, height);
      }

   if (index >= ctx->Const.MaxViewports) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glScissorIndexed: index (%d) >= MaxViewports (%d)",
                  index, ctx->Const.MaxViewports);
      return;
   }

   _mesa_set_scissori(ctx, index, left, bottom, width, height);
}

/**
 * Define the scissor box.
 *
 * \param x, y coordinates of the scissor box lower-left corner.
 * \param width width of the scissor box.
 * \param height height of the scissor box.
 *
 * \sa glScissor().
 *
 * Verifies the parameters call _mesa_set_scissori to do the work.
 */
void GLAPIENTRY
_mesa_ScissorIndexedv(GLuint index, const GLint * v)
{
   struct gl_scissor_rect *p = (struct gl_scissor_rect *) v;
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_API)
      _mesa_debug(ctx, "glScissorIndexedv(%d, %d, %d, %d, %d)\n", index,
                  p->X, p->Y, p->Width, p->Height);

   if (p->Width < 0 || p->Height < 0) {
         _mesa_error(ctx, GL_INVALID_VALUE,
                     "glScissorIndexedv: index (%d) width or height < 0 (%d, %d)",
                     index, p->Width, p->Height);
      }

   if (index >= ctx->Const.MaxViewports) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glScissorIndexedv: index (%d) >= MaxViewports (%d)",
                  index, ctx->Const.MaxViewports);
      return;
   }

   _mesa_set_scissori(ctx, index, p->X, p->Y, p->Width, p->Height);
}

/**
 * Initialize the context's scissor state.
 * \param ctx  the GL context.
 */
void
_mesa_init_scissor(struct gl_context *ctx)
{
   GLint i;

   /* Scissor group */
   ctx->Scissor.EnableFlags = 0;

   for (i = 0; i < MAX_VIEWPORTS; i++) {
      ctx->Scissor.ScissorArray[i].X = 0;
      ctx->Scissor.ScissorArray[i].Y = 0;
      ctx->Scissor.ScissorArray[i].Width = 0;
      ctx->Scissor.ScissorArray[i].Height = 0;
   }
}
