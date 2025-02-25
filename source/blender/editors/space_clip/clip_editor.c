/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2011 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup spclip
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/types.h>

#ifndef WIN32
#  include <unistd.h>
#else
#  include <io.h>
#endif

#include "MEM_guardedalloc.h"

#include "DNA_mask_types.h"

#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_rect.h"
#include "BLI_task.h"
#include "BLI_utildefines.h"

#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_lib_id.h"
#include "BKE_main.h"
#include "BKE_mask.h"
#include "BKE_movieclip.h"
#include "BKE_tracking.h"

#include "IMB_colormanagement.h"
#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "ED_clip.h"
#include "ED_mask.h"
#include "ED_screen.h"
#include "ED_select_utils.h"

#include "WM_api.h"
#include "WM_types.h"

#include "UI_view2d.h"

#include "clip_intern.h" /* own include */

#include "PIL_time.h"

/* -------------------------------------------------------------------- */
/** \name Operator Poll Functions
 * \{ */

bool ED_space_clip_poll(bContext *C)
{
  SpaceClip *sc = CTX_wm_space_clip(C);

  if (sc && sc->clip) {
    return true;
  }

  return false;
}

bool ED_space_clip_view_clip_poll(bContext *C)
{
  SpaceClip *sc = CTX_wm_space_clip(C);

  if (sc) {
    return sc->view == SC_VIEW_CLIP;
  }

  return false;
}

bool ED_space_clip_tracking_poll(bContext *C)
{
  SpaceClip *sc = CTX_wm_space_clip(C);

  if (sc && sc->clip) {
    return ED_space_clip_check_show_trackedit(sc);
  }

  return false;
}

bool ED_space_clip_maskedit_poll(bContext *C)
{
  SpaceClip *sc = CTX_wm_space_clip(C);

  if (sc && sc->clip) {
    return ED_space_clip_check_show_maskedit(sc);
  }

  return false;
}

bool ED_space_clip_maskedit_mask_poll(bContext *C)
{
  if (ED_space_clip_maskedit_poll(C)) {
    MovieClip *clip = CTX_data_edit_movieclip(C);

    if (clip) {
      SpaceClip *sc = CTX_wm_space_clip(C);

      return sc->mask_info.mask != NULL;
    }
  }

  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Common Editing Functions
 * \{ */

void ED_space_clip_get_size(SpaceClip *sc, int *width, int *height)
{
  if (sc->clip) {
    BKE_movieclip_get_size(sc->clip, &sc->user, width, height);
  }
  else {
    *width = *height = IMG_SIZE_FALLBACK;
  }
}

void ED_space_clip_get_size_fl(SpaceClip *sc, float size[2])
{
  int size_i[2];
  ED_space_clip_get_size(sc, &size_i[0], &size_i[1]);
  size[0] = size_i[0];
  size[1] = size_i[1];
}

void ED_space_clip_get_zoom(SpaceClip *sc, ARegion *region, float *zoomx, float *zoomy)
{
  int width, height;

  ED_space_clip_get_size(sc, &width, &height);

  *zoomx = (float)(BLI_rcti_size_x(&region->winrct) + 1) /
           (BLI_rctf_size_x(&region->v2d.cur) * width);
  *zoomy = (float)(BLI_rcti_size_y(&region->winrct) + 1) /
           (BLI_rctf_size_y(&region->v2d.cur) * height);
}

void ED_space_clip_get_aspect(SpaceClip *sc, float *aspx, float *aspy)
{
  MovieClip *clip = ED_space_clip_get_clip(sc);

  if (clip) {
    BKE_movieclip_get_aspect(clip, aspx, aspy);
  }
  else {
    *aspx = *aspy = 1.0f;
  }

  if (*aspx < *aspy) {
    *aspy = *aspy / *aspx;
    *aspx = 1.0f;
  }
  else {
    *aspx = *aspx / *aspy;
    *aspy = 1.0f;
  }
}

void ED_space_clip_get_aspect_dimension_aware(SpaceClip *sc, float *aspx, float *aspy)
{
  int w, h;

  /* most of tools does not require aspect to be returned with dimensions correction
   * due to they're invariant to this stuff, but some transformation tools like rotation
   * should be aware of aspect correction caused by different resolution in different
   * directions.
   * mainly this is used for transformation stuff
   */

  if (!sc->clip) {
    *aspx = 1.0f;
    *aspy = 1.0f;

    return;
  }

  ED_space_clip_get_aspect(sc, aspx, aspy);
  BKE_movieclip_get_size(sc->clip, &sc->user, &w, &h);

  *aspx *= (float)w;
  *aspy *= (float)h;

  if (*aspx < *aspy) {
    *aspy = *aspy / *aspx;
    *aspx = 1.0f;
  }
  else {
    *aspx = *aspx / *aspy;
    *aspy = 1.0f;
  }
}

/* return current frame number in clip space */
int ED_space_clip_get_clip_frame_number(SpaceClip *sc)
{
  MovieClip *clip = ED_space_clip_get_clip(sc);

  /* Caller must ensure space does have a valid clip, otherwise it will crash, see T45017. */
  return BKE_movieclip_remap_scene_to_clip_frame(clip, sc->user.framenr);
}

ImBuf *ED_space_clip_get_buffer(SpaceClip *sc)
{
  if (sc->clip) {
    ImBuf *ibuf;

    ibuf = BKE_movieclip_get_postprocessed_ibuf(sc->clip, &sc->user, sc->postproc_flag);

    if (ibuf && (ibuf->rect || ibuf->rect_float)) {
      return ibuf;
    }

    if (ibuf) {
      IMB_freeImBuf(ibuf);
    }
  }

  return NULL;
}

ImBuf *ED_space_clip_get_stable_buffer(SpaceClip *sc, float loc[2], float *scale, float *angle)
{
  if (sc->clip) {
    ImBuf *ibuf;

    ibuf = BKE_movieclip_get_stable_ibuf(
        sc->clip, &sc->user, loc, scale, angle, sc->postproc_flag);

    if (ibuf && (ibuf->rect || ibuf->rect_float)) {
      return ibuf;
    }

    if (ibuf) {
      IMB_freeImBuf(ibuf);
    }
  }

  return NULL;
}

bool ED_space_clip_get_position(struct SpaceClip *sc,
                                struct ARegion *ar,
                                int mval[2],
                                float fpos[2])
{
  ImBuf *ibuf = ED_space_clip_get_buffer(sc);
  if (!ibuf) {
    return false;
  }

  /* map the mouse coords to the backdrop image space */
  ED_clip_mouse_pos(sc, ar, mval, fpos);

  IMB_freeImBuf(ibuf);
  return true;
}

/* Returns color in linear space, matching ED_space_image_color_sample(). */
bool ED_space_clip_color_sample(SpaceClip *sc, ARegion *region, int mval[2], float r_col[3])
{
  ImBuf *ibuf;
  float fx, fy, co[2];
  bool ret = false;

  ibuf = ED_space_clip_get_buffer(sc);
  if (!ibuf) {
    return false;
  }

  /* map the mouse coords to the backdrop image space */
  ED_clip_mouse_pos(sc, region, mval, co);

  fx = co[0];
  fy = co[1];

  if (fx >= 0.0f && fy >= 0.0f && fx < 1.0f && fy < 1.0f) {
    const float *fp;
    uchar *cp;
    int x = (int)(fx * ibuf->x), y = (int)(fy * ibuf->y);

    CLAMP(x, 0, ibuf->x - 1);
    CLAMP(y, 0, ibuf->y - 1);

    if (ibuf->rect_float) {
      fp = (ibuf->rect_float + (ibuf->channels) * (y * ibuf->x + x));
      copy_v3_v3(r_col, fp);
      ret = true;
    }
    else if (ibuf->rect) {
      cp = (uchar *)(ibuf->rect + y * ibuf->x + x);
      rgb_uchar_to_float(r_col, cp);
      IMB_colormanagement_colorspace_to_scene_linear_v3(r_col, ibuf->rect_colorspace);
      ret = true;
    }
  }

  IMB_freeImBuf(ibuf);

  return ret;
}

void ED_clip_update_frame(const Main *mainp, int cfra)
{
  /* image window, compo node users */
  for (wmWindowManager *wm = mainp->wm.first; wm; wm = wm->id.next) { /* only 1 wm */
    LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
      bScreen *screen = WM_window_get_active_screen(win);

      LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
        if (area->spacetype == SPACE_CLIP) {
          SpaceClip *sc = area->spacedata.first;

          sc->scopes.ok = false;

          BKE_movieclip_user_set_frame(&sc->user, cfra);
        }
      }
    }
  }
}

bool ED_clip_view_selection(const bContext *C, ARegion *UNUSED(region), bool fit)
{
  float offset_x, offset_y;
  float zoom;
  if (!clip_view_calculate_view_selection(C, fit, &offset_x, &offset_y, &zoom)) {
    return false;
  }

  SpaceClip *sc = CTX_wm_space_clip(C);
  sc->xof = offset_x;
  sc->yof = offset_y;
  sc->zoom = zoom;

  return true;
}

void ED_clip_select_all(SpaceClip *sc, int action, bool *r_has_selection)
{
  MovieClip *clip = ED_space_clip_get_clip(sc);
  const int framenr = ED_space_clip_get_clip_frame_number(sc);
  MovieTracking *tracking = &clip->tracking;
  MovieTrackingTrack *track = NULL;            /* selected track */
  MovieTrackingPlaneTrack *plane_track = NULL; /* selected plane track */
  MovieTrackingMarker *marker;
  ListBase *tracksbase = BKE_tracking_get_active_tracks(tracking);
  ListBase *plane_tracks_base = BKE_tracking_get_active_plane_tracks(tracking);
  bool has_selection = false;

  if (action == SEL_TOGGLE) {
    action = SEL_SELECT;

    for (track = tracksbase->first; track; track = track->next) {
      if (TRACK_VIEW_SELECTED(sc, track)) {
        marker = BKE_tracking_marker_get(track, framenr);

        if (MARKER_VISIBLE(sc, track, marker)) {
          action = SEL_DESELECT;
          break;
        }
      }
    }

    for (plane_track = plane_tracks_base->first; plane_track; plane_track = plane_track->next) {
      if (PLANE_TRACK_VIEW_SELECTED(plane_track)) {
        action = SEL_DESELECT;
        break;
      }
    }
  }

  for (track = tracksbase->first; track; track = track->next) {
    if ((track->flag & TRACK_HIDDEN) == 0) {
      marker = BKE_tracking_marker_get(track, framenr);

      if (MARKER_VISIBLE(sc, track, marker)) {
        switch (action) {
          case SEL_SELECT:
            track->flag |= SELECT;
            track->pat_flag |= SELECT;
            track->search_flag |= SELECT;
            break;
          case SEL_DESELECT:
            track->flag &= ~SELECT;
            track->pat_flag &= ~SELECT;
            track->search_flag &= ~SELECT;
            break;
          case SEL_INVERT:
            track->flag ^= SELECT;
            track->pat_flag ^= SELECT;
            track->search_flag ^= SELECT;
            break;
        }
      }
    }

    if (TRACK_VIEW_SELECTED(sc, track)) {
      has_selection = true;
    }
  }

  for (plane_track = plane_tracks_base->first; plane_track; plane_track = plane_track->next) {
    if ((plane_track->flag & PLANE_TRACK_HIDDEN) == 0) {
      switch (action) {
        case SEL_SELECT:
          plane_track->flag |= SELECT;
          break;
        case SEL_DESELECT:
          plane_track->flag &= ~SELECT;
          break;
        case SEL_INVERT:
          plane_track->flag ^= SELECT;
          break;
      }
      if (plane_track->flag & SELECT) {
        has_selection = true;
      }
    }
  }

  if (r_has_selection) {
    *r_has_selection = has_selection;
  }
}

void ED_clip_point_undistorted_pos(SpaceClip *sc, const float co[2], float r_co[2])
{
  copy_v2_v2(r_co, co);

  if (sc->user.render_flag & MCLIP_PROXY_RENDER_UNDISTORT) {
    MovieClip *clip = ED_space_clip_get_clip(sc);
    float aspy = 1.0f / clip->tracking.camera.pixel_aspect;
    int width, height;

    BKE_movieclip_get_size(sc->clip, &sc->user, &width, &height);

    r_co[0] *= width;
    r_co[1] *= height * aspy;

    BKE_tracking_undistort_v2(&clip->tracking, width, height, r_co, r_co);

    r_co[0] /= width;
    r_co[1] /= height * aspy;
  }
}

void ED_clip_point_stable_pos(
    SpaceClip *sc, ARegion *region, float x, float y, float *xr, float *yr)
{
  int sx, sy, width, height;
  float zoomx, zoomy, pos[3], imat[4][4];

  ED_space_clip_get_zoom(sc, region, &zoomx, &zoomy);
  ED_space_clip_get_size(sc, &width, &height);

  UI_view2d_view_to_region(&region->v2d, 0.0f, 0.0f, &sx, &sy);

  pos[0] = (x - sx) / zoomx;
  pos[1] = (y - sy) / zoomy;
  pos[2] = 0.0f;

  invert_m4_m4(imat, sc->stabmat);
  mul_v3_m4v3(pos, imat, pos);

  *xr = pos[0] / width;
  *yr = pos[1] / height;

  if (sc->user.render_flag & MCLIP_PROXY_RENDER_UNDISTORT) {
    MovieClip *clip = ED_space_clip_get_clip(sc);
    MovieTracking *tracking = &clip->tracking;
    float aspy = 1.0f / tracking->camera.pixel_aspect;
    float tmp[2] = {*xr * width, *yr * height * aspy};

    BKE_tracking_distort_v2(tracking, width, height, tmp, tmp);

    *xr = tmp[0] / width;
    *yr = tmp[1] / (height * aspy);
  }
}

/**
 * \brief the reverse of #ED_clip_point_stable_pos(), gets the marker region coords.
 * better name here? view_to_track / track_to_view or so?
 */
void ED_clip_point_stable_pos__reverse(SpaceClip *sc,
                                       ARegion *region,
                                       const float co[2],
                                       float r_co[2])
{
  float zoomx, zoomy;
  float pos[3];
  int width, height;
  int sx, sy;

  UI_view2d_view_to_region(&region->v2d, 0.0f, 0.0f, &sx, &sy);
  ED_space_clip_get_size(sc, &width, &height);
  ED_space_clip_get_zoom(sc, region, &zoomx, &zoomy);

  ED_clip_point_undistorted_pos(sc, co, pos);
  pos[2] = 0.0f;

  /* untested */
  mul_v3_m4v3(pos, sc->stabmat, pos);

  r_co[0] = (pos[0] * width * zoomx) + (float)sx;
  r_co[1] = (pos[1] * height * zoomy) + (float)sy;
}

/* takes event->mval */
void ED_clip_mouse_pos(SpaceClip *sc, ARegion *region, const int mval[2], float co[2])
{
  ED_clip_point_stable_pos(sc, region, mval[0], mval[1], &co[0], &co[1]);
}

bool ED_space_clip_check_show_trackedit(SpaceClip *sc)
{
  if (sc) {
    return sc->mode == SC_MODE_TRACKING;
  }

  return false;
}

bool ED_space_clip_check_show_maskedit(SpaceClip *sc)
{
  if (sc) {
    return sc->mode == SC_MODE_MASKEDIT;
  }

  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Clip Editing Functions
 * \{ */

MovieClip *ED_space_clip_get_clip(SpaceClip *sc)
{
  return sc->clip;
}

void ED_space_clip_set_clip(bContext *C, bScreen *screen, SpaceClip *sc, MovieClip *clip)
{
  MovieClip *old_clip;
  bool old_clip_visible = false;

  if (!screen && C) {
    screen = CTX_wm_screen(C);
  }

  old_clip = sc->clip;
  sc->clip = clip;

  id_us_ensure_real((ID *)sc->clip);

  if (screen && sc->view == SC_VIEW_CLIP) {
    ScrArea *area;
    SpaceLink *sl;

    for (area = screen->areabase.first; area; area = area->next) {
      for (sl = area->spacedata.first; sl; sl = sl->next) {
        if (sl->spacetype == SPACE_CLIP) {
          SpaceClip *cur_sc = (SpaceClip *)sl;

          if (cur_sc != sc) {
            if (cur_sc->view == SC_VIEW_CLIP) {
              if (cur_sc->clip == old_clip) {
                old_clip_visible = true;
              }
            }
            else {
              if (ELEM(cur_sc->clip, old_clip, NULL)) {
                cur_sc->clip = clip;
              }
            }
          }
        }
      }
    }
  }

  /* If clip is no longer visible on screen, free memory used by its cache */
  if (old_clip && old_clip != clip && !old_clip_visible) {
    BKE_movieclip_clear_cache(old_clip);
  }

  if (C) {
    WM_event_add_notifier(C, NC_MOVIECLIP | NA_SELECTED, sc->clip);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Masking Editing Functions
 * \{ */

Mask *ED_space_clip_get_mask(SpaceClip *sc)
{
  return sc->mask_info.mask;
}

void ED_space_clip_set_mask(bContext *C, SpaceClip *sc, Mask *mask)
{
  sc->mask_info.mask = mask;

  id_us_ensure_real((ID *)sc->mask_info.mask);

  if (C) {
    WM_event_add_notifier(C, NC_MASK | NA_SELECTED, mask);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pre-Fetching Functions
 * \{ */

typedef struct PrefetchJob {
  /* Clip into which cache the frames will be prefetched into. */
  MovieClip *clip;

  /* Local copy of the clip which is used to decouple reading in a way which does not require
   * threading lock which might "conflict" with the main thread,
   *
   * Used, for example, for animation prefetching (`clip->anim` can not be used from multiple
   * threads and main thread might need it). */
  MovieClip *clip_local;

  int start_frame, current_frame, end_frame;
  short render_size, render_flag;
} PrefetchJob;

typedef struct PrefetchQueue {
  int initial_frame, current_frame, start_frame, end_frame;
  short render_size, render_flag;

  /* If true pre-fetching goes forward in time,
   * otherwise it goes backwards in time (starting from current frame).
   */
  bool forward;

  SpinLock spin;

  short *stop;
  short *do_update;
  float *progress;
} PrefetchQueue;

/* check whether pre-fetching is allowed */
static bool check_prefetch_break(void)
{
  return G.is_break;
}

/* read file for specified frame number to the memory */
static uchar *prefetch_read_file_to_memory(
    MovieClip *clip, int current_frame, short render_size, short render_flag, size_t *r_size)
{
  MovieClipUser user = {0};
  user.framenr = current_frame;
  user.render_size = render_size;
  user.render_flag = render_flag;

  char name[FILE_MAX];
  BKE_movieclip_filename_for_frame(clip, &user, name);

  int file = BLI_open(name, O_BINARY | O_RDONLY, 0);
  if (file == -1) {
    return NULL;
  }

  const size_t size = BLI_file_descriptor_size(file);
  if (size < 1) {
    close(file);
    return NULL;
  }

  uchar *mem = MEM_mallocN(size, "movieclip prefetch memory file");
  if (mem == NULL) {
    close(file);
    return NULL;
  }

  if (read(file, mem, size) != size) {
    close(file);
    MEM_freeN(mem);
    return NULL;
  }

  *r_size = size;

  close(file);

  return mem;
}

/* find first uncached frame within prefetching frame range */
static int prefetch_find_uncached_frame(MovieClip *clip,
                                        int from_frame,
                                        int end_frame,
                                        short render_size,
                                        short render_flag,
                                        short direction)
{
  int current_frame;
  MovieClipUser user = {0};

  user.render_size = render_size;
  user.render_flag = render_flag;

  if (direction > 0) {
    for (current_frame = from_frame; current_frame <= end_frame; current_frame++) {
      user.framenr = current_frame;

      if (!BKE_movieclip_has_cached_frame(clip, &user)) {
        break;
      }
    }
  }
  else {
    for (current_frame = from_frame; current_frame >= end_frame; current_frame--) {
      user.framenr = current_frame;

      if (!BKE_movieclip_has_cached_frame(clip, &user)) {
        break;
      }
    }
  }

  return current_frame;
}

/* get memory buffer for first uncached frame within prefetch frame range */
static uchar *prefetch_thread_next_frame(PrefetchQueue *queue,
                                         MovieClip *clip,
                                         size_t *r_size,
                                         int *r_current_frame)
{
  uchar *mem = NULL;

  BLI_spin_lock(&queue->spin);
  if (!*queue->stop && !check_prefetch_break() &&
      IN_RANGE_INCL(queue->current_frame, queue->start_frame, queue->end_frame)) {
    int current_frame;

    if (queue->forward) {
      current_frame = prefetch_find_uncached_frame(clip,
                                                   queue->current_frame + 1,
                                                   queue->end_frame,
                                                   queue->render_size,
                                                   queue->render_flag,
                                                   1);
      /* switch direction if read frames from current up to scene end frames */
      if (current_frame > queue->end_frame) {
        queue->current_frame = queue->initial_frame;
        queue->forward = false;
      }
    }

    if (!queue->forward) {
      current_frame = prefetch_find_uncached_frame(clip,
                                                   queue->current_frame - 1,
                                                   queue->start_frame,
                                                   queue->render_size,
                                                   queue->render_flag,
                                                   -1);
    }

    if (IN_RANGE_INCL(current_frame, queue->start_frame, queue->end_frame)) {
      int frames_processed;

      mem = prefetch_read_file_to_memory(
          clip, current_frame, queue->render_size, queue->render_flag, r_size);

      *r_current_frame = current_frame;

      queue->current_frame = current_frame;

      if (queue->forward) {
        frames_processed = queue->current_frame - queue->initial_frame;
      }
      else {
        frames_processed = (queue->end_frame - queue->initial_frame) +
                           (queue->initial_frame - queue->current_frame);
      }

      *queue->do_update = 1;
      *queue->progress = (float)frames_processed / (queue->end_frame - queue->start_frame);
    }
  }
  BLI_spin_unlock(&queue->spin);

  return mem;
}

static void prefetch_task_func(TaskPool *__restrict pool, void *task_data)
{
  PrefetchQueue *queue = (PrefetchQueue *)BLI_task_pool_user_data(pool);
  MovieClip *clip = (MovieClip *)task_data;
  uchar *mem;
  size_t size;
  int current_frame;

  while ((mem = prefetch_thread_next_frame(queue, clip, &size, &current_frame))) {
    ImBuf *ibuf;
    MovieClipUser user = {0};
    int flag = IB_rect | IB_multilayer | IB_alphamode_detect | IB_metadata;
    int result;
    char *colorspace_name = NULL;
    const bool use_proxy = (clip->flag & MCLIP_USE_PROXY) &&
                           (queue->render_size != MCLIP_PROXY_RENDER_SIZE_FULL);

    user.framenr = current_frame;
    user.render_size = queue->render_size;
    user.render_flag = queue->render_flag;

    /* Proxies are stored in the display space. */
    if (!use_proxy) {
      colorspace_name = clip->colorspace_settings.name;
    }

    ibuf = IMB_ibImageFromMemory(mem, size, flag, colorspace_name, "prefetch frame");
    if (ibuf == NULL) {
      continue;
    }
    BKE_movieclip_convert_multilayer_ibuf(ibuf);

    result = BKE_movieclip_put_frame_if_possible(clip, &user, ibuf);

    IMB_freeImBuf(ibuf);

    MEM_freeN(mem);

    if (!result) {
      /* no more space in the cache, stop reading frames */
      *queue->stop = 1;
      break;
    }
  }
}

static void start_prefetch_threads(MovieClip *clip,
                                   int start_frame,
                                   int current_frame,
                                   int end_frame,
                                   short render_size,
                                   short render_flag,
                                   short *stop,
                                   short *do_update,
                                   float *progress)
{
  int tot_thread = BLI_task_scheduler_num_threads();

  /* initialize queue */
  PrefetchQueue queue;
  BLI_spin_init(&queue.spin);

  queue.current_frame = current_frame;
  queue.initial_frame = current_frame;
  queue.start_frame = start_frame;
  queue.end_frame = end_frame;
  queue.render_size = render_size;
  queue.render_flag = render_flag;
  queue.forward = 1;

  queue.stop = stop;
  queue.do_update = do_update;
  queue.progress = progress;

  TaskPool *task_pool = BLI_task_pool_create(&queue, TASK_PRIORITY_LOW);
  for (int i = 0; i < tot_thread; i++) {
    BLI_task_pool_push(task_pool, prefetch_task_func, clip, false, NULL);
  }
  BLI_task_pool_work_and_wait(task_pool);
  BLI_task_pool_free(task_pool);

  BLI_spin_end(&queue.spin);
}

/* NOTE: Reading happens from `clip_local` into `clip->cache`. */
static bool prefetch_movie_frame(MovieClip *clip,
                                 MovieClip *clip_local,
                                 int frame,
                                 short render_size,
                                 short render_flag,
                                 short *stop)
{
  MovieClipUser user = {0};

  if (check_prefetch_break() || *stop) {
    return false;
  }

  user.framenr = frame;
  user.render_size = render_size;
  user.render_flag = render_flag;

  if (!BKE_movieclip_has_cached_frame(clip, &user)) {
    ImBuf *ibuf = BKE_movieclip_anim_ibuf_for_frame_no_lock(clip_local, &user);

    if (ibuf) {
      int result;

      result = BKE_movieclip_put_frame_if_possible(clip, &user, ibuf);

      if (!result) {
        /* no more space in the cache, we could stop prefetching here */
        *stop = 1;
      }

      IMB_freeImBuf(ibuf);
    }
    else {
      /* error reading frame, fair enough stop attempting further reading */
      *stop = 1;
    }
  }

  return true;
}

static void do_prefetch_movie(MovieClip *clip,
                              MovieClip *clip_local,
                              int start_frame,
                              int current_frame,
                              int end_frame,
                              short render_size,
                              short render_flag,
                              short *stop,
                              short *do_update,
                              float *progress)
{
  int frame;
  int frames_processed = 0;

  /* read frames starting from current frame up to scene end frame */
  for (frame = current_frame; frame <= end_frame; frame++) {
    if (!prefetch_movie_frame(clip, clip_local, frame, render_size, render_flag, stop)) {
      return;
    }

    frames_processed++;

    *do_update = 1;
    *progress = (float)frames_processed / (end_frame - start_frame);
  }

  /* read frames starting from current frame up to scene start frame */
  for (frame = current_frame; frame >= start_frame; frame--) {
    if (!prefetch_movie_frame(clip, clip_local, frame, render_size, render_flag, stop)) {
      return;
    }

    frames_processed++;

    *do_update = 1;
    *progress = (float)frames_processed / (end_frame - start_frame);
  }
}

static void prefetch_startjob(void *pjv, short *stop, short *do_update, float *progress)
{
  PrefetchJob *pj = pjv;

  if (pj->clip->source == MCLIP_SRC_SEQUENCE) {
    /* read sequence files in multiple threads */
    start_prefetch_threads(pj->clip,
                           pj->start_frame,
                           pj->current_frame,
                           pj->end_frame,
                           pj->render_size,
                           pj->render_flag,
                           stop,
                           do_update,
                           progress);
  }
  else if (pj->clip->source == MCLIP_SRC_MOVIE) {
    /* read movie in a single thread */
    do_prefetch_movie(pj->clip,
                      pj->clip_local,
                      pj->start_frame,
                      pj->current_frame,
                      pj->end_frame,
                      pj->render_size,
                      pj->render_flag,
                      stop,
                      do_update,
                      progress);
  }
  else {
    BLI_assert(!"Unknown movie clip source when prefetching frames");
  }
}

static void prefetch_freejob(void *pjv)
{
  PrefetchJob *pj = pjv;

  MovieClip *clip_local = pj->clip_local;
  if (clip_local != NULL) {
    BKE_libblock_free_datablock(&clip_local->id, 0);
    BKE_libblock_free_data(&clip_local->id, false);
    BLI_assert(!clip_local->id.py_instance); /* Or call #BKE_libblock_free_data_py. */
    MEM_freeN(clip_local);
  }

  MEM_freeN(pj);
}

static int prefetch_get_start_frame(const bContext *C)
{
  Scene *scene = CTX_data_scene(C);

  return SFRA;
}

static int prefetch_get_final_frame(const bContext *C)
{
  Scene *scene = CTX_data_scene(C);
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  int end_frame;

  /* check whether all the frames from prefetch range are cached */
  end_frame = EFRA;

  if (clip->len) {
    end_frame = min_ii(end_frame, SFRA + clip->len - 1);
  }

  return end_frame;
}

/* returns true if early out is possible */
static bool prefetch_check_early_out(const bContext *C)
{
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  int first_uncached_frame, end_frame;
  int clip_len;

  if (clip == NULL) {
    return true;
  }

  clip_len = BKE_movieclip_get_duration(clip);

  /* check whether all the frames from prefetch range are cached */
  end_frame = prefetch_get_final_frame(C);

  first_uncached_frame = prefetch_find_uncached_frame(
      clip, sc->user.framenr, end_frame, sc->user.render_size, sc->user.render_flag, 1);

  if (first_uncached_frame > end_frame || first_uncached_frame == clip_len) {
    int start_frame = prefetch_get_start_frame(C);

    first_uncached_frame = prefetch_find_uncached_frame(
        clip, sc->user.framenr, start_frame, sc->user.render_size, sc->user.render_flag, -1);

    if (first_uncached_frame < start_frame) {
      return true;
    }
  }

  return false;
}

void clip_start_prefetch_job(const bContext *C)
{
  wmJob *wm_job;
  PrefetchJob *pj;
  SpaceClip *sc = CTX_wm_space_clip(C);

  if (prefetch_check_early_out(C)) {
    return;
  }

  wm_job = WM_jobs_get(CTX_wm_manager(C),
                       CTX_wm_window(C),
                       CTX_data_scene(C),
                       "Prefetching",
                       WM_JOB_PROGRESS,
                       WM_JOB_TYPE_CLIP_PREFETCH);

  /* create new job */
  pj = MEM_callocN(sizeof(PrefetchJob), "prefetch job");
  pj->clip = ED_space_clip_get_clip(sc);
  pj->start_frame = prefetch_get_start_frame(C);
  pj->current_frame = sc->user.framenr;
  pj->end_frame = prefetch_get_final_frame(C);
  pj->render_size = sc->user.render_size;
  pj->render_flag = sc->user.render_flag;

  /* Create a local copy of the clip, so that video file (clip->anim) access can happen without
   * acquiring the lock which will interfere with the main thread. */
  if (pj->clip->source == MCLIP_SRC_MOVIE) {
    BKE_id_copy_ex(NULL, (ID *)&pj->clip->id, (ID **)&pj->clip_local, LIB_ID_COPY_LOCALIZE);
  }

  WM_jobs_customdata_set(wm_job, pj, prefetch_freejob);
  WM_jobs_timer(wm_job, 0.2, NC_MOVIECLIP | ND_DISPLAY, 0);
  WM_jobs_callbacks(wm_job, prefetch_startjob, NULL, NULL, NULL);

  G.is_break = false;

  /* and finally start the job */
  WM_jobs_start(CTX_wm_manager(C), wm_job);
}

void ED_clip_view_lock_state_store(const bContext *C, ClipViewLockState *state)
{
  SpaceClip *space_clip = CTX_wm_space_clip(C);
  BLI_assert(space_clip != NULL);

  state->offset_x = space_clip->xof;
  state->offset_y = space_clip->yof;
  state->zoom = space_clip->zoom;

  state->lock_offset_x = 0.0f;
  state->lock_offset_y = 0.0f;

  if ((space_clip->flag & SC_LOCK_SELECTION) == 0) {
    return;
  }

  if (!clip_view_calculate_view_selection(
          C, false, &state->offset_x, &state->offset_y, &state->zoom)) {
    return;
  }

  state->lock_offset_x = space_clip->xlockof;
  state->lock_offset_y = space_clip->ylockof;
}

void ED_clip_view_lock_state_restore_no_jump(const bContext *C, const ClipViewLockState *state)
{
  SpaceClip *space_clip = CTX_wm_space_clip(C);
  BLI_assert(space_clip != NULL);

  if ((space_clip->flag & SC_LOCK_SELECTION) == 0) {
    return;
  }

  float offset_x, offset_y;
  float zoom;
  if (!clip_view_calculate_view_selection(C, false, &offset_x, &offset_y, &zoom)) {
    return;
  }

  space_clip->xlockof = state->offset_x + state->lock_offset_x - offset_x;
  space_clip->ylockof = state->offset_y + state->lock_offset_y - offset_y;
}

/** \} */
