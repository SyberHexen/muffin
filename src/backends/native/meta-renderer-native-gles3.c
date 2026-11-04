/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2017 Red Hat
 * Copyright (c) 2018 DisplayLink (UK) Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 */

#include "config.h"

#define GL_GLEXT_PROTOTYPES

#include "backends/native/meta-renderer-native-gles3.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <gio/gio.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "backends/meta-egl-ext.h"
#include "backends/meta-gles3.h"
#include "backends/meta-gles3-table.h"

/*
 * GL/gl.h being included may conflit with gl3.h on some architectures.
 * Make sure that hasn't happened on any architecture.
 */
#ifdef GL_VERSION_1_1
#error "Somehow included OpenGL headers when we shouldn't have"
#endif

/*
 * The shared buffer comes from another GPU, and some drivers (NVIDIA) only
 * accept such dma-buf images as GL_TEXTURE_EXTERNAL_OES textures:
 * glEGLImageTargetTexture2DOES (GL_TEXTURE_2D, ...) fails with
 * GL_INVALID_OPERATION and the output stays black. External textures cannot
 * be framebuffer attachments, so draw a textured quad instead of using
 * glBlitFramebuffer. GL_OES_EGL_image_external is required by
 * init_secondary_gpu_data_gpu().
 */
static const char blit_vertex_shader[] =
  "#version 100\n"
  "attribute vec2 position;\n"
  "varying vec2 texcoord;\n"
  "void main ()\n"
  "{\n"
  "  gl_Position = vec4 (position, 0.0, 1.0);\n"
  "  texcoord = vec2 (position.x + 1.0, 1.0 - position.y) * 0.5;\n"
  "}\n";

static const char blit_fragment_shader[] =
  "#version 100\n"
  "#extension GL_OES_EGL_image_external : require\n"
  "precision mediump float;\n"
  "uniform samplerExternalOES tex;\n"
  "varying vec2 texcoord;\n"
  "void main ()\n"
  "{\n"
  "  gl_FragColor = texture2D (tex, texcoord);\n"
  "}\n";

static GLuint
compile_shader (GLenum       type,
                const char  *source,
                GError     **error)
{
  GLuint shader;
  GLint compiled = GL_FALSE;

  shader = glCreateShader (type);
  glShaderSource (shader, 1, &source, NULL);
  glCompileShader (shader);
  glGetShaderiv (shader, GL_COMPILE_STATUS, &compiled);
  if (!compiled)
    {
      char log[512] = "";

      glGetShaderInfoLog (shader, sizeof (log), NULL, log);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to compile blit shader: %s", log);
      glDeleteShader (shader);
      return 0;
    }

  return shader;
}

/* One program per secondary GPU context; contexts do not share objects. */
static GHashTable *blit_programs;

static GLuint
ensure_blit_program (EGLContext   egl_context,
                     GError     **error)
{
  GLuint program;
  GLuint vertex_shader;
  GLuint fragment_shader;
  GLint linked = GL_FALSE;

  if (!blit_programs)
    blit_programs = g_hash_table_new (NULL, NULL);

  /* A destroyed context's address can be reused by a new one. */
  program = GPOINTER_TO_UINT (g_hash_table_lookup (blit_programs, egl_context));
  if (program && glIsProgram (program))
    return program;

  vertex_shader = compile_shader (GL_VERTEX_SHADER, blit_vertex_shader, error);
  if (!vertex_shader)
    return 0;

  fragment_shader = compile_shader (GL_FRAGMENT_SHADER, blit_fragment_shader,
                                    error);
  if (!fragment_shader)
    {
      glDeleteShader (vertex_shader);
      return 0;
    }

  program = glCreateProgram ();
  glAttachShader (program, vertex_shader);
  glAttachShader (program, fragment_shader);
  glBindAttribLocation (program, 0, "position");
  glLinkProgram (program);
  glDeleteShader (vertex_shader);
  glDeleteShader (fragment_shader);

  glGetProgramiv (program, GL_LINK_STATUS, &linked);
  if (!linked)
    {
      char log[512] = "";

      glGetProgramInfoLog (program, sizeof (log), NULL, log);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to link blit program: %s", log);
      glDeleteProgram (program);
      return 0;
    }

  glUseProgram (program);
  glUniform1i (glGetUniformLocation (program, "tex"), 0);

  g_hash_table_insert (blit_programs, egl_context, GUINT_TO_POINTER (program));
  return program;
}

static gboolean
paint_egl_image (MetaGles3    *gles3,
                 EGLContext    egl_context,
                 EGLImageKHR   egl_image,
                 int           width,
                 int           height,
                 GError      **error)
{
  static const GLfloat quad[] = {
    -1.0f, -1.0f,
     1.0f, -1.0f,
    -1.0f,  1.0f,
     1.0f,  1.0f,
  };
  GLuint program;
  MetaGles3Table *table;
  GLuint texture;
  gboolean ret;

  meta_gles3_clear_error (gles3);

  program = ensure_blit_program (egl_context, error);
  if (!program)
    return FALSE;

  GLBAS (gles3, glActiveTexture, (GL_TEXTURE0));
  GLBAS (gles3, glGenTextures, (1, &texture));
  GLBAS (gles3, glBindTexture, (GL_TEXTURE_EXTERNAL_OES, texture));

  table = meta_gles3_get_table (gles3);
  meta_gles3_ensure_loaded (gles3, (gpointer *) &table->glEGLImageTargetTexture2DOES,
                            "glEGLImageTargetTexture2DOES");
  table->glEGLImageTargetTexture2DOES (GL_TEXTURE_EXTERNAL_OES, egl_image);
  if (!meta_gles3_validate (gles3, error))
    {
      g_prefix_error (error, "Failed to bind the shared buffer: ");
      glDeleteTextures (1, &texture);
      return FALSE;
    }

  GLBAS (gles3, glTexParameteri, (GL_TEXTURE_EXTERNAL_OES,
                                  GL_TEXTURE_MAG_FILTER, GL_NEAREST));
  GLBAS (gles3, glTexParameteri, (GL_TEXTURE_EXTERNAL_OES,
                                  GL_TEXTURE_MIN_FILTER, GL_NEAREST));
  GLBAS (gles3, glTexParameteri, (GL_TEXTURE_EXTERNAL_OES,
                                  GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
  GLBAS (gles3, glTexParameteri, (GL_TEXTURE_EXTERNAL_OES,
                                  GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));

  GLBAS (gles3, glBindFramebuffer, (GL_FRAMEBUFFER, 0));
  GLBAS (gles3, glViewport, (0, 0, width, height));
  GLBAS (gles3, glDisable, (GL_BLEND));
  GLBAS (gles3, glDisable, (GL_SCISSOR_TEST));
  GLBAS (gles3, glUseProgram, (program));
  GLBAS (gles3, glBindBuffer, (GL_ARRAY_BUFFER, 0));
  GLBAS (gles3, glVertexAttribPointer, (0, 2, GL_FLOAT, GL_FALSE, 0, quad));
  GLBAS (gles3, glEnableVertexAttribArray, (0));

  /* GLBAS only warns; fail the blit if the draw does, so the caller does not
   * scan out an unpainted buffer. */
  glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
  ret = meta_gles3_validate (gles3, error);

  GLBAS (gles3, glDisableVertexAttribArray, (0));
  GLBAS (gles3, glDeleteTextures, (1, &texture));

  return ret;
}

/*
 * The shared buffer is rendered by one GPU and read by another, and the
 * reading driver may not honour the buffer's implicit fences (NVIDIA does
 * not). Without this the secondary GPU samples a partly rendered frame, and
 * the primary GPU can clear the buffer for its next frame while the copy is
 * still reading it - both show up as black blocks. Wait for the rendering on
 * the GPU, and attach the copy's completion to the buffer as a read fence so
 * the primary's next write waits for it. Falls back to waiting on the CPU.
 */
typedef struct
{
  gboolean available;
  PFNEGLCREATESYNCKHRPROC create_sync;
  PFNEGLDESTROYSYNCKHRPROC destroy_sync;
  PFNEGLWAITSYNCKHRPROC wait_sync;
  PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_native_fence_fd;
} NativeFenceSync;

static const NativeFenceSync *
get_native_fence_sync (EGLDisplay egl_display)
{
  static EGLDisplay cached_display = EGL_NO_DISPLAY;
  static NativeFenceSync sync;
  const char *extensions;

  if (egl_display == cached_display)
    return sync.available ? &sync : NULL;

  cached_display = egl_display;
  memset (&sync, 0, sizeof (sync));

  extensions = eglQueryString (egl_display, EGL_EXTENSIONS);
  if (!extensions ||
      !strstr (extensions, "EGL_ANDROID_native_fence_sync") ||
      !strstr (extensions, "EGL_KHR_wait_sync"))
    return NULL;

  sync.create_sync = (gpointer) eglGetProcAddress ("eglCreateSyncKHR");
  sync.destroy_sync = (gpointer) eglGetProcAddress ("eglDestroySyncKHR");
  sync.wait_sync = (gpointer) eglGetProcAddress ("eglWaitSyncKHR");
  sync.dup_native_fence_fd =
    (gpointer) eglGetProcAddress ("eglDupNativeFenceFDANDROID");
  sync.available = sync.create_sync && sync.destroy_sync &&
                   sync.wait_sync && sync.dup_native_fence_fd;

  return sync.available ? &sync : NULL;
}

static void
wait_for_shared_bo_rendering (EGLDisplay egl_display,
                              int        dmabuf_fd)
{
  const NativeFenceSync *sync = get_native_fence_sync (egl_display);
  struct pollfd pfd = { .fd = dmabuf_fd, .events = POLLIN };

  if (sync)
    {
      /* The fences a reader has to wait for, i.e. the pending writes. */
      struct dma_buf_export_sync_file export_sync = {
        .flags = DMA_BUF_SYNC_READ,
        .fd = -1,
      };

      if (ioctl (dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export_sync) == 0)
        {
          EGLint attribs[] = {
            EGL_SYNC_NATIVE_FENCE_FD_ANDROID, export_sync.fd,
            EGL_NONE
          };
          EGLSyncKHR egl_sync;

          /* On success EGL owns the fd. */
          egl_sync = sync->create_sync (egl_display,
                                        EGL_SYNC_NATIVE_FENCE_ANDROID,
                                        attribs);
          if (egl_sync != EGL_NO_SYNC_KHR)
            {
              sync->wait_sync (egl_display, egl_sync, 0);
              sync->destroy_sync (egl_display, egl_sync);
              return;
            }
          close (export_sync.fd);
        }
    }

  /* A dma-buf polls readable once its pending writes are done. */
  poll (&pfd, 1, 1000);
}

static void
fence_shared_bo_read (EGLDisplay egl_display,
                      int        dmabuf_fd)
{
  const NativeFenceSync *sync = get_native_fence_sync (egl_display);

  if (sync)
    {
      EGLint attribs[] = { EGL_NONE };
      EGLSyncKHR egl_sync;
      int fence_fd = -1;

      egl_sync = sync->create_sync (egl_display,
                                    EGL_SYNC_NATIVE_FENCE_ANDROID,
                                    attribs);
      if (egl_sync != EGL_NO_SYNC_KHR)
        {
          /* The fence fd exists once the sync command is flushed. */
          glFlush ();
          fence_fd = sync->dup_native_fence_fd (egl_display, egl_sync);
          sync->destroy_sync (egl_display, egl_sync);
        }

      if (fence_fd >= 0)
        {
          struct dma_buf_import_sync_file import_sync = {
            .flags = DMA_BUF_SYNC_READ,
            .fd = fence_fd,
          };
          int ret;

          ret = ioctl (dmabuf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE,
                       &import_sync);
          close (fence_fd);
          if (ret == 0)
            return;
        }
    }

  glFinish ();
}

gboolean
meta_renderer_native_gles3_blit_shared_bo (MetaEgl        *egl,
                                           MetaGles3      *gles3,
                                           EGLDisplay      egl_display,
                                           EGLContext      egl_context,
                                           EGLSurface      egl_surface,
                                           struct gbm_bo  *shared_bo,
                                           GError        **error)
{
  int shared_bo_fd;
  unsigned int width;
  unsigned int height;
  uint32_t i, n_planes;
  uint32_t strides[4] = { 0 };
  uint32_t offsets[4] = { 0 };
  uint64_t modifiers[4] = { 0 };
  int fds[4] = { -1, -1, -1, -1 };
  uint32_t format;
  EGLImageKHR egl_image;
  gboolean use_modifiers;
  gboolean ret;

  shared_bo_fd = gbm_bo_get_fd (shared_bo);
  if (shared_bo_fd < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to export gbm_bo: %s", strerror (errno));
      return FALSE;
    }

  width = gbm_bo_get_width (shared_bo);
  height = gbm_bo_get_height (shared_bo);
  format = gbm_bo_get_format (shared_bo);

  n_planes = gbm_bo_get_plane_count (shared_bo);
  for (i = 0; i < n_planes; i++)
    {
      strides[i] = gbm_bo_get_stride_for_plane (shared_bo, i);
      offsets[i] = gbm_bo_get_offset (shared_bo, i);
      modifiers[i] = gbm_bo_get_modifier (shared_bo);
      fds[i] = shared_bo_fd;
    }

  /* Workaround for https://gitlab.gnome.org/GNOME/muffin/issues/18 */
  if (modifiers[0] == DRM_FORMAT_MOD_LINEAR ||
      modifiers[0] == DRM_FORMAT_MOD_INVALID)
    use_modifiers = FALSE;
  else
    use_modifiers = TRUE;

  egl_image = meta_egl_create_dmabuf_image (egl,
                                            egl_display,
                                            width,
                                            height,
                                            format,
                                            n_planes,
                                            fds,
                                            strides,
                                            offsets,
                                            use_modifiers ? modifiers : NULL,
                                            error);
  if (!egl_image)
    {
      close (shared_bo_fd);
      return FALSE;
    }

  wait_for_shared_bo_rendering (egl_display, shared_bo_fd);

  ret = paint_egl_image (gles3, egl_context, egl_image, width, height, error);
  if (ret)
    fence_shared_bo_read (egl_display, shared_bo_fd);

  meta_egl_destroy_image (egl, egl_display, egl_image, NULL);
  close (shared_bo_fd);

  return ret;
}
