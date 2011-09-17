/*
 * Copyright (C) 2011 Felipe Contreras
 * Copyright (C) 2011 x264 project
 *
 * Author: Felipe Contreras <felipe.contreras@gmail.com>
 *
 * This file may be used under the terms of the GNU General Public License
 * version 2.
 */

#include <gst/gst.h>
#include <gst/base/gstcollectpads.h>

#include <stdint.h>

GstDebugCategory *gst_xssim_debug;

#define GST_CAT_DEFAULT gst_xssim_debug

#define SINK_CAPS \
	"video/x-raw-yuv, " \
"format = (fourcc) I420"

#define SRC_CAPS \
	"video/x-raw-gray, " \
"width = (int) [ 1, MAX ], " \
"height = (int) [ 1, MAX ], " \
"framerate = (fraction) [ 0/1, MAX ], " \
"bpp = (int) 8, " \
"depth = (int) 8 "

static GstStaticPadTemplate src_template =
GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS(SRC_CAPS));

static GstStaticPadTemplate sink_template =
GST_STATIC_PAD_TEMPLATE("sink%d", GST_PAD_SINK, GST_PAD_SOMETIMES, GST_STATIC_CAPS(SINK_CAPS));

static GstElementClass *parent_class;

static guint got_results_signal;

struct gst_xssim {
	GstElement parent;

	GstCollectPads *collect;

	int width, height;

	GstPad *srcpad;
	GstCollectData *sink0, *sink1;

	double avg, min, max;
	long count;
};

struct gst_xssim_class {
	GstElementClass parent_class;
};

static void ssim_4x4x2_core(const uint8_t *pix1, int stride1,
		const uint8_t *pix2, int stride2,
		int sums[2][4])
{
    for (int z = 0; z < 2; z++) {
        uint32_t s1 = 0, s2 = 0, ss = 0, s12 = 0;
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                int a = pix1[x + y * stride1];
                int b = pix2[x + y * stride2];
                s1 += a;
                s2 += b;
                ss += a * a;
                ss += b * b;
                s12 += a * b;
            }
	}
        sums[z][0] = s1;
        sums[z][1] = s2;
        sums[z][2] = ss;
        sums[z][3] = s12;
        pix1 += 4;
        pix2 += 4;
    }
}

static float ssim_end1(int s1, int s2, int ss, int s12)
{
    static const int ssim_c1 = (int)(.01 * .01 * 255 * 255 * 64 + .5);
    static const int ssim_c2 = (int)(.03 * .03 * 255 * 255 * 64 * 63 + .5);
    int vars = ss * 64 - s1 * s1 - s2 * s2;
    int covar = s12 * 64 - s1 * s2;
    return (float)(2 * s1 * s2 + ssim_c1) * (float)(2 * covar + ssim_c2)
         / ((float)(s1 * s1 + s2 * s2 + ssim_c1) * (float)(vars + ssim_c2));
}

static float ssim_end4(int sum0[5][4], int sum1[5][4], int width)
{
	float ssim = 0.0;
	for (int i = 0; i < width; i++)
		ssim += ssim_end1(sum0[i][0] + sum0[i + 1][0] + sum1[i][0] + sum1[i + 1][0],
				sum0[i][1] + sum0[i + 1][1] + sum1[i][1] + sum1[i + 1][1],
				sum0[i][2] + sum0[i + 1][2] + sum1[i][2] + sum1[i + 1][2],
				sum0[i][3] + sum0[i + 1][3] + sum1[i][3] + sum1[i + 1][3]);
	return ssim;
}

#define XCHG(type,a,b) do { type t = a; a = b; b = t; } while(0)

static float x264_pixel_ssim_wxh(uint8_t *pix1, int stride1,
		uint8_t *pix2, int stride2,
		int width, int height, void *buf, int *cnt)
{
	int z = 0;
	float ssim = 0.0;
	int (*sum0)[4] = buf;
	int (*sum1)[4] = sum0 + (width >> 2) + 3;
	width >>= 2;
	height >>= 2;
	for (int y = 1; y < height; y++) {
		for(; z <= y; z++) {
			XCHG(void *, sum0, sum1);
			for (int x = 0; x < width; x += 2)
				ssim_4x4x2_core(&pix1[4 * (x + z * stride1)], stride1,
						&pix2[4 * (x + z * stride2)], stride2, &sum0[x]);
		}
		for (int x = 0; x < width - 1; x += 4)
			ssim += ssim_end4(sum0 + x, sum1 + x, MIN(4, width - x - 1));
	}
	*cnt = (height - 1) * (width - 1);
	return ssim;
}

static GstFlowReturn collected(GstCollectPads *pads, void *user_data)
{
	struct gst_xssim *self = user_data;
	GstFlowReturn ret;
	GstBuffer *inbuf0, *inbuf1;
	GstBuffer *outbuf;
	double ssim_y;
	gfloat r;
	long c;
	int ssim_cnt;

	inbuf0 = gst_collect_pads_pop(pads, self->sink0);
	inbuf1 = gst_collect_pads_pop(pads, self->sink1);
	if (!inbuf0 || !inbuf1) {
		gst_pad_push_event(self->srcpad, gst_event_new_eos());
		g_signal_emit(self, got_results_signal, 0, self->avg);
		ret = GST_FLOW_UNEXPECTED;
		goto leave;
	}

	outbuf = gst_buffer_new_and_alloc(GST_ROUND_UP_4(self->width) * self->height);
	gst_buffer_set_caps(outbuf, gst_pad_get_fixed_caps_func(self->srcpad));

	gst_buffer_copy_metadata(outbuf, inbuf1, GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS);

	r = x264_pixel_ssim_wxh(
			inbuf0->data, self->width,
			inbuf1->data, self->width,
			self->width, self->height, outbuf->data, &ssim_cnt);
	ssim_y = r / ssim_cnt;

	c = self->count++;
	self->avg = (self->avg * c + ssim_y) / (c + 1);
	if (ssim_y < self->min)
		self->min = ssim_y;
	if (ssim_y > self->max)
		self->max = ssim_y;

	GST_DEBUG_OBJECT(self, "SSIM is %f", ssim_y);

	ret = gst_pad_push(self->srcpad, outbuf);

leave:
	if (inbuf0)
		gst_buffer_unref(inbuf0);
	if (inbuf1)
		gst_buffer_unref(inbuf1);
	return ret;
}

static gboolean setcaps(GstPad *pad, GstCaps *caps)
{
	struct gst_xssim *self;
	GstStructure *struc;
	gint width, height, fps_n, fps_d;

	self = ((struct gst_xssim *)((GstObject *)pad)->parent);

	GST_INFO_OBJECT(self, "setting caps on pad %s to %" GST_PTR_FORMAT, GST_PAD_NAME(pad), caps);

	struc = gst_caps_get_structure(caps, 0);
	gst_structure_get_int(struc, "width", &width);
	gst_structure_get_int(struc, "height", &height);
	gst_structure_get_fraction(struc, "framerate", &fps_n, &fps_d);

	if (!self->srcpad->caps) {
		GstCaps *new_caps;

		GST_OBJECT_LOCK(self);
		self->width = width;
		self->height = height;
		GST_OBJECT_UNLOCK(self);

		new_caps = gst_caps_new_simple("video/x-raw-gray",
				"width", G_TYPE_INT, width,
				"height", G_TYPE_INT, height,
				"framerate", GST_TYPE_FRACTION, fps_n, fps_d,
				"bpp", G_TYPE_INT, 8,
				"depth", G_TYPE_INT, 8,
				NULL);

		GST_INFO_OBJECT(self, "setting caps on pad %s to %" GST_PTR_FORMAT, GST_PAD_NAME(self->srcpad), new_caps);
		gst_pad_set_caps(self->srcpad, new_caps);
	} else {
		gboolean equal;

		GST_OBJECT_LOCK(self);
		equal = self->width == width && self->height == height;
		GST_OBJECT_UNLOCK(self);

		if (!equal) {
			GST_ERROR_OBJECT(self, "missmatch on negotiated caps for %s", GST_PAD_NAME(self->srcpad));
			return FALSE;
		}
	}

	return gst_pad_set_caps(pad, caps);
}

static void instance_init(GTypeInstance *instance, void *g_class)
{
	struct gst_xssim *self;
	GstElementClass *element_class;
	GstPadTemplate *template;
	GstPad *pad;

	element_class = GST_ELEMENT_CLASS(g_class);
	self = (struct gst_xssim *)instance;

	self->collect = gst_collect_pads_new();
	gst_collect_pads_set_function(self->collect, collected, self);

	template = gst_element_class_get_pad_template(element_class, "src");
	self->srcpad = gst_pad_new_from_template(template, "src");

	gst_element_add_pad(GST_ELEMENT(self), self->srcpad);

	gst_pad_use_fixed_caps(self->srcpad);

	template = gst_element_class_get_pad_template(element_class, "sink%d");
	pad = gst_pad_new_from_template(template, "sink0");

	gst_pad_set_setcaps_function(pad, setcaps);
	self->sink0 = gst_collect_pads_add_pad(self->collect, pad, sizeof(GstCollectData));

	gst_element_add_pad(GST_ELEMENT(self), pad);

	pad = gst_pad_new_from_template(template, "sink1");
	gst_pad_set_setcaps_function(pad, setcaps);
	self->sink1 = gst_collect_pads_add_pad(self->collect, pad, sizeof(GstCollectData));

	gst_element_add_pad(GST_ELEMENT(self), pad);
}

static GstStateChangeReturn gst_ssim_change_state(GstElement *element, GstStateChange transition)
{
	struct gst_xssim *self = (struct gst_xssim *)element;

	switch (transition) {
	case GST_STATE_CHANGE_READY_TO_PAUSED:
		self->avg = 0.0;
		self->min = 1.0;
		self->max = 0.0;
		gst_collect_pads_start(self->collect);
		break;
	case GST_STATE_CHANGE_PAUSED_TO_READY:
		gst_collect_pads_stop(self->collect);
		GST_INFO_OBJECT(self, "avg=%f, min=%f, max=%f", self->avg, self->min, self->max);
		break;
	default:
		break;
	}

	return parent_class->change_state(element, transition);
}

static void finalize(GObject *object)
{
	struct gst_xssim *self = (struct gst_xssim *)object;
	gst_object_unref(self->collect);
	((GObjectClass *)parent_class)->finalize(object);
}

static void class_init(void *g_class, void *class_data)
{
	GObjectClass *gobject_class = g_class;
	GstElementClass *gstelement_class = g_class;

	parent_class = g_type_class_peek_parent(g_class);

	gobject_class->finalize = finalize;

	gst_element_class_add_pad_template(gstelement_class,
			gst_static_pad_template_get(&src_template));
	gst_element_class_add_pad_template(gstelement_class,
			gst_static_pad_template_get(&sink_template));

	gst_element_class_set_details_simple(gstelement_class, "XSSIM",
			"Filter/Converter/Video",
			"Calculate SSIM for video streams",
			"Felipe Contreras <felipe.contreras@gmail.com>");

	gstelement_class->change_state = gst_ssim_change_state;

	got_results_signal = g_signal_new("got_results", G_TYPE_FROM_CLASS(g_class),
			G_SIGNAL_RUN_LAST, 0, NULL, NULL,
			g_cclosure_marshal_VOID__DOUBLE, G_TYPE_NONE, 1, G_TYPE_DOUBLE);
}

static GType get_type(void)
{
	static GType type;

	if (G_UNLIKELY(type == 0)) {
		GTypeInfo type_info = {
			.class_size = sizeof(struct gst_xssim_class),
			.class_init = class_init,
			.instance_size = sizeof(struct gst_xssim),
			.instance_init = instance_init,
		};

		type = g_type_register_static(GST_TYPE_ELEMENT, "GstXssim", &type_info, 0);
#ifndef GST_DISABLE_GST_DEBUG
		gst_xssim_debug = _gst_debug_category_new("xssim", 0, "XSSIM stuff");
#endif
	}

	return type;
}

static gboolean plugin_init(GstPlugin *plugin)
{
	if (!gst_element_register(plugin, "xssim", GST_RANK_NONE, get_type()))
		return FALSE;

	return TRUE;
}

GstPluginDesc gst_plugin_desc = {
	.major_version = 0,
	.minor_version = 10,
	.name = "xssim",
	.description = (gchar *) "Efficient SSIM tool",
	.plugin_init = plugin_init,
	.version = VERSION,
	.license = "GPL",
	.source = "gst-xssim",
	.package = "none",
	.origin = "none",
};
