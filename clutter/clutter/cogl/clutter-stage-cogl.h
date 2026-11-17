#ifndef __CLUTTER_STAGE_COGL_H__
#define __CLUTTER_STAGE_COGL_H__

#include <cairo.h>
#include <clutter/clutter-backend.h>
#include <clutter/clutter-stage.h>

#include "clutter/clutter-stage-window.h"

G_BEGIN_DECLS

#define CLUTTER_TYPE_STAGE_COGL                  (_clutter_stage_cogl_get_type ())
#define CLUTTER_STAGE_COGL(obj)                  (G_TYPE_CHECK_INSTANCE_CAST ((obj), CLUTTER_TYPE_STAGE_COGL, ClutterStageCogl))
#define CLUTTER_IS_STAGE_COGL(obj)               (G_TYPE_CHECK_INSTANCE_TYPE ((obj), CLUTTER_TYPE_STAGE_COGL))
#define CLUTTER_STAGE_COGL_CLASS(klass)          (G_TYPE_CHECK_CLASS_CAST ((klass), CLUTTER_TYPE_STAGE_COGL, ClutterStageCoglClass))
#define CLUTTER_IS_STAGE_COGL_CLASS(klass)       (G_TYPE_CHECK_CLASS_TYPE ((klass), CLUTTER_TYPE_STAGE_COGL))
#define CLUTTER_STAGE_COGL_GET_CLASS(obj)        (G_TYPE_INSTANCE_GET_CLASS ((obj), CLUTTER_TYPE_STAGE_COGL, ClutterStageCoglClass))

typedef struct _ClutterStageCogl         ClutterStageCogl;
typedef struct _ClutterStageCoglClass    ClutterStageCoglClass;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ClutterStageCogl, g_object_unref)

#define CLUTTER_TYPE_STAGE_VIEW_COGL (clutter_stage_view_cogl_get_type ())
CLUTTER_EXPORT
G_DECLARE_DERIVABLE_TYPE (ClutterStageViewCogl, clutter_stage_view_cogl,
                          CLUTTER, STAGE_VIEW_COGL,
                          ClutterStageView)

struct _ClutterStageViewCoglClass
{
  ClutterStageViewClass parent_class;
};

struct _ClutterStageCogl
{
  GObject parent_instance;

 /* the stage wrapper */
  ClutterStage *wrapper;

  /* back pointer to the backend */
  ClutterBackend *backend;

  /* A stage update that no view's damage times (an animation's next step,
   * a relayout, events): asked for, its time and the next presentation
   * time it was timed for; each view has its own (ClutterStageViewCogl) */
  gboolean update_wanted;
  /* Asked to update at once (clutter_stage_skip_sync_delay()) */
  gboolean update_immediately;
  gint64 update_time;
  int64_t last_update_time;
  int64_t next_presentation_time;
  /* Presentation time of the earliest view or stage update */
  int64_t earliest_presentation_time;

  /* We only enable clipped redraws after 2 frames, since we've seen
   * a lot of drivers can struggle to get going and may output some
   * junk frames to start with. */
  unsigned int frame_count;

  gint last_sync_delay;
};

struct _ClutterStageCoglClass
{
  GObjectClass parent_class;
};

CLUTTER_EXPORT
GType _clutter_stage_cogl_get_type (void) G_GNUC_CONST;

CLUTTER_EXPORT
void _clutter_stage_cogl_presented (ClutterStageCogl *stage_cogl,
                                    CoglFrameEvent    frame_event,
                                    ClutterFrameInfo *frame_info);

CLUTTER_EXPORT
void _clutter_stage_cogl_view_presented (ClutterStageCogl *stage_cogl,
                                         ClutterStageView *view,
                                         CoglFrameEvent    frame_event,
                                         ClutterFrameInfo *frame_info);

G_END_DECLS

#endif /* __CLUTTER_STAGE_COGL_H__ */
