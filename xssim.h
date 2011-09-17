#ifndef XSSIM_H
#define XSSIM_H

#include <glib-object.h>

struct gst_xssim;

#define GST_XSSIM_TYPE (gst_xssim_get_type())

GType gst_xssim_get_type(void);

void ssim_get_results(struct gst_xssim *self, double *avg, double *min, double *max);

#endif
