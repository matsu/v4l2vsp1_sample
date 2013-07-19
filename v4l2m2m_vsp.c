/*
 *  V4L2 video capture example
 *
 *  This program can be used and distributed without restrictions.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <getopt.h>             /* getopt_long() */

#include <fcntl.h>              /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <asm/types.h>          /* for videodev2.h */

#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#include <linux/v4l2-mediabus.h>

#define N_BUFFERS 2
#define CLEAR(x) memset (&(x), 0, sizeof (x))

typedef enum {
        IO_METHOD_READ,
        IO_METHOD_MMAP,
        IO_METHOD_USERPTR,
} io_method;

struct buffer {
        void *                  start;
        size_t                  length;
};

enum {
	OUT = 0,
	CAP = 1,
};

static char *           dev_name[2]     = { NULL, NULL };
static char *           rwpf_name[2]     = { NULL, NULL };
static io_method        io              = IO_METHOD_MMAP;
static uint32_t		format[2]	= { V4L2_PIX_FMT_NV12M, V4L2_PIX_FMT_RGB565 };
static enum v4l2_mbus_pixelcode code[2]	= { V4L2_MBUS_FMT_AYUV8_1X32, V4L2_MBUS_FMT_ARGB8888_1X32 };
static unsigned int     n_planes[2]     = { 2, 1 };
static int		width[2]	= { 1280, 1280 };
static int		height[2]	= { 720, 720 };
static int              v4lout_fd       = -1;
static int              v4lcap_fd       = -1;
static int              v4lsub_fd[2]    = { -1, -1 };
static int              input_fd        = -1;
static int              output_fd       = -1;
struct buffer           buffers[2][N_BUFFERS][VIDEO_MAX_PLANES];
struct v4l2_plane       planes[2][VIDEO_MAX_PLANES];
static unsigned int     n_buffers[2]    = { 0, 0 };
static const char *	ocstring[2]	= { "OUT" , "CAP" };

static void
errno_exit                      (const char *           s, const char *s2)
{
	if (s2)
		fprintf (stderr, "%s%s error %d, %s\n",
			 s, s2, errno, strerror (errno));
	else
		fprintf (stderr, "%s error %d, %s\n",
			 s, errno, strerror (errno));

        exit (EXIT_FAILURE);
}

static int
xioctl                          (int                    fd,
                                 int                    request,
                                 void *                 arg)
{
        int r;

        do r = ioctl (fd, request, arg);
        while (-1 == r && EINTR == errno);

        return r;
}

static void
process_image                   (const void *p, size_t len)
{
	printf("O %d bytes\n", len);
        fflush (stdout);
	if (output_fd >= 0)
		write(output_fd, p, len);
}

static int
read_frame                      (int fd, int index, enum v4l2_buf_type buftype)
{
        struct v4l2_buffer buf;
        unsigned int i;

        switch (io) {
        case IO_METHOD_READ:
                break;

        case IO_METHOD_MMAP:
                CLEAR (buf);

                buf.type = buftype;
                buf.memory = V4L2_MEMORY_MMAP;
		buf.m.planes = planes[index];
		buf.length = n_planes[index];

                if (-1 == xioctl (fd, VIDIOC_DQBUF, &buf)) {
                        switch (errno) {
                        case EAGAIN:
                                return 0;

                        case EIO:
                                /* Could ignore EIO, see spec. */

                                /* fall through */

                        default:
                                errno_exit ("VIDIOC_DQBUF for ", ocstring[index]);
                        }
                }

                assert (buf.index < n_buffers[index]);

		if (index == CAP) {
			for (i=0; i<n_planes[index]; i++)
				process_image (buffers[index][buf.index][i].start,
					       buffers[index][buf.index][i].length);
		        fputc ('I', stdout);
			fflush (stdout);
		} else if (input_fd >= 0 /* && (index == OUT) */) {
			for (i=0; i<n_planes[index]; i++)
				read(input_fd, buffers[index][buf.index][i].start,
				     buffers[index][buf.index][i].length);
		        fputc ('o', stdout);
			fflush (stdout);
		}

                if (-1 == xioctl (fd, VIDIOC_QBUF, &buf))
                        errno_exit ("VIDIOC_QBUF for ", ocstring[index]);
                break;

        case IO_METHOD_USERPTR:
                break;
        }

        return 1;
}

static void
mainloop                        (void)
{
        unsigned int count;

        count = 100;

        while (count-- > 0) {
                for (;;) {
                        fd_set fds;
                        struct timeval tv;
                        int r;

                        FD_ZERO (&fds);
                        FD_SET (v4lcap_fd, &fds);

                        /* Timeout. */
                        tv.tv_sec = 2;
                        tv.tv_usec = 0;

                        r = select (v4lcap_fd + 1, &fds, NULL, NULL, &tv);

                        if (-1 == r) {
                                if (EINTR == errno)
                                        continue;

                                errno_exit ("select for cap", NULL);
                        }

                        if (0 == r) {
                                fprintf (stderr, "select timeout\n");
                                exit (EXIT_FAILURE);
                        }

			/* dequeue a capture buffer and read it */
			r = read_frame (v4lcap_fd, CAP, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
			if (!r)
				continue; /* EAGAIN - continue select loop. */

			/* dequeue an output buffer and refill it */
			do {
				r = read_frame (v4lout_fd, OUT, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
			} while (!r);

			break;
                }
        }
	printf("finishing...\n");
}

static void
stop_capturing                  (int fd, int index, enum v4l2_buf_type buftype)
{
        switch (io) {
        case IO_METHOD_READ:
                /* Nothing to do. */
                break;

        case IO_METHOD_MMAP:
        case IO_METHOD_USERPTR:

		printf("stop streaming... ");fflush(stdout);
                if (-1 == xioctl (fd, VIDIOC_STREAMOFF, &buftype))
                        errno_exit ("VIDIOC_STREAMOFF for ", dev_name[index]);
		printf("done.\n");
                break;
        }
}

static void
queue_buffers                (int fd, int index, enum v4l2_buf_type buftype)
{
        unsigned int i;

        switch (io) {
        case IO_METHOD_READ:
                /* Nothing to do. */
                break;

        case IO_METHOD_MMAP:
                for (i = 0; i < n_buffers[index]; ++i) {
                        struct v4l2_buffer buf;
			int j;

                        CLEAR (buf);

                        buf.type        = buftype;
                        buf.memory      = V4L2_MEMORY_MMAP;
                        buf.index       = i;
			buf.m.planes    = planes[index];
			buf.length      = n_planes[index];

			if ((index == OUT) && (input_fd >= 0))
				for (j=0; j<n_planes[index]; j++)
					read(input_fd, buffers[index][i][j].start,
					     buffers[index][i][j].length);
                        if (-1 == xioctl (fd, VIDIOC_QBUF, &buf))
                                errno_exit ("VIDIOC_QBUF for ", ocstring[index]);
			else
				printf("%s[%d] queued\n", ocstring[index], i);
                }

                break;

        case IO_METHOD_USERPTR:
                break;
        }
}

static void
start_capturing                 (int fd, int index, enum v4l2_buf_type buftype)
{
        unsigned int i;

        if (-1 == xioctl (fd, VIDIOC_STREAMON, &buftype))
		errno_exit ("VIDIOC_STREAMON for ", ocstring[index]);
}

static void
uninit_device                   (int index)
{
        unsigned int i, j;

        switch (io) {
        case IO_METHOD_READ:
                break;

        case IO_METHOD_MMAP:
		printf("unmapping ... ");
                for (i = 0; i < n_buffers[index]; ++i) {
			for (j = 0; j < n_planes[index]; ++j)
				if (-1 == munmap (buffers[index][i][j].start, buffers[index][i][j].length))
					errno_exit ("munmap for ", dev_name[index]);
			printf("[%d] ", i);fflush(stdout);
		}
		printf("done.\n");

                break;

        case IO_METHOD_USERPTR:
                break;
        }
}

static void
init_mmap                       (int fd, int index, enum v4l2_buf_type buftype, int n_bufs)
{
        struct v4l2_requestbuffers req;

	/* input buffer */
        CLEAR (req);

        req.count               = n_bufs;
        req.type                = buftype;
        req.memory              = V4L2_MEMORY_MMAP;

        if (-1 == xioctl (fd, VIDIOC_REQBUFS, &req)) {
                if (EINVAL == errno) {
                        fprintf (stderr, "%s does not support "
                                 "memory mapping\n", dev_name[index]);
			errno_exit("VIDIOC_REQBUFS for ", dev_name[index]);
                } else {
                        errno_exit ("VIDIOC_REQBUFS for ", dev_name[index]);
                }
        }

	printf("req.count = %d\n", req.count);
	n_bufs = req.count;

        for (n_buffers[index] = 0; n_buffers[index] < n_bufs; ++n_buffers[index]) {
                struct v4l2_buffer buf;
		int i;

                CLEAR (buf);
                memset((void *)planes[index], 0,
		       sizeof(struct v4l2_plane) * VIDEO_MAX_PLANES);

                buf.type        = buftype;
                buf.memory      = V4L2_MEMORY_MMAP;
                buf.index       = n_buffers[index];
		buf.m.planes    = planes[index];
		buf.length      = n_planes[index];

                if (-1 == xioctl (fd, VIDIOC_QUERYBUF, &buf))
                        errno_exit ("VIDIOC_QUERYBUF for ", dev_name[index]);

		printf("n_planes = %d\n", buf.length);
		n_planes[index] = buf.length;
		for (i=0; i<n_planes[index]; i++) {
			printf("m.plane[%d].length = %d, m.plane[%d].m.mem_offset = %08x\n",
			       i, planes[index][i].length,
			       i, planes[index][i].m.mem_offset);

			buffers[index][n_buffers[index]][i].length = planes[index][i].length;
			buffers[index][n_buffers[index]][i].start =
				mmap (NULL /* start anywhere */,
				      planes[index][i].length,
				      PROT_READ | PROT_WRITE /* required */,
				      MAP_SHARED /* recommended */,
				      fd, planes[index][i].m.mem_offset);

			if (MAP_FAILED == buffers[index][n_buffers[index]][i].start)
				errno_exit ("mmap for ", dev_name[index]);
		}
        }
	printf("done\n");
}

static int fgets_with_openclose(char *fname, char *buf, size_t maxlen)
{
	FILE *fp;
	char *s;

	if ((fp = fopen(fname, "r")) != NULL) {
		s = fgets(buf, maxlen, fp);
		fclose(fp);
		return (s != NULL) ? strlen(buf) : 0;
	} else {
		return -1;
	}
}

static void
init_entity_pad (int fd, int index, uint32_t pad, uint32_t width, uint32_t height, uint32_t code)
{
	struct v4l2_subdev_format sfmt;

        CLEAR (sfmt);

        sfmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	sfmt.pad = pad;
	sfmt.format.width = width;
	sfmt.format.height = height;
	sfmt.format.code = code;
	sfmt.format.field = V4L2_FIELD_NONE;
	sfmt.format.colorspace = V4L2_COLORSPACE_SRGB;
	
        if (-1 == xioctl (fd, VIDIOC_SUBDEV_S_FMT, &sfmt))
                errno_exit ("VIDIOC_SUBDEV_S_FMT for ", rwpf_name[index]);
}

	
static void
init_device                     (int fd, int index, uint32_t captype, enum v4l2_buf_type buftype)
{
        struct v4l2_capability cap;
        struct v4l2_cropcap cropcap;
        struct v4l2_crop crop;
        struct v4l2_format fmt;
        unsigned int min, i;
	char *p;

        if (-1 == xioctl (fd, VIDIOC_QUERYCAP, &cap)) {
                if (EINVAL == errno) {
                        fprintf (stderr, "%s is no V4L2 device\n",
                                 dev_name[index]);
                        exit (EXIT_FAILURE);
                } else {
                        errno_exit ("VIDIOC_QUERYCAP for ", dev_name[index]);
                }
        }

	/* look for a counterpart */
	rwpf_name[index] = strdup(cap.card);
	p = strstr(rwpf_name[index], index == OUT ? "input" : "output");
	if (p == NULL) {
		printf("index =%d\n", index);
	        errno_exit ("unknown module name : ", cap.card);
	}
	*(p - 1) = '\0';
	printf("MODULE NAME[%d] = %s\n", index, rwpf_name[index]);

	for (i = 0; i < 255; i++) {
		char path[256], subdev_name[256];

		snprintf(path, 255, "/sys/class/video4linux/v4l-subdev%d/name", i);
		if (fgets_with_openclose(path,
					 subdev_name,
					 255) < 0) {
			printf("NO counterpart n = %d\n", i);
			break;
		}
		if (strncmp(subdev_name, rwpf_name[index], strlen(rwpf_name[index])) == 0) {
			snprintf(path, 255, "/dev/v4l-subdev%d", i);
			printf("counterpart %s\n", path);
			v4lsub_fd[index] = open (path, O_RDWR /* required | O_NONBLOCK */, 0);
			if (v4lsub_fd[index] < 0) {
		                fprintf (stderr, "Cannot open '%s': %d, %s\n",
		                         path, errno, strerror (errno));
		                exit (EXIT_FAILURE);
			}
			break;
		}
	}

        if (!(cap.capabilities & captype)) {
                fprintf (stderr, "%s is not suitable device (%08x != %08x)\n",
                         dev_name[index], cap.capabilities, captype);
                exit (EXIT_FAILURE);
        }

        switch (io) {
        case IO_METHOD_READ:
                if (!(cap.capabilities & V4L2_CAP_READWRITE)) {
                        fprintf (stderr, "%s does not support read i/o\n",
                                 dev_name[index]);
                        exit (EXIT_FAILURE);
                }

                break;

        case IO_METHOD_MMAP:
        case IO_METHOD_USERPTR:
                if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
                        fprintf (stderr, "%s does not support streaming i/o\n",
                                 dev_name[index]);
                        exit (EXIT_FAILURE);
                }

                break;
        }


        /* Select video input, video standard and tune here. */


        CLEAR (cropcap);

        cropcap.type = buftype;

        if (0 == xioctl (fd, VIDIOC_CROPCAP, &cropcap)) {
                crop.type = buftype;
                crop.c = cropcap.defrect; /* reset to default */

                if (-1 == xioctl (fd, VIDIOC_S_CROP, &crop)) {
                        switch (errno) {
                        case EINVAL:
                                /* Cropping not supported. */
                                break;
                        default:
                                /* Errors ignored. */
                                break;
                        }
                }
        } else {
                /* Errors ignored. */
        }


        CLEAR (fmt);

        fmt.type                = buftype;
        fmt.fmt.pix_mp.width       = width[index];
        fmt.fmt.pix_mp.height      = height[index];
        fmt.fmt.pix_mp.pixelformat = format[index];
        fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;

        if (-1 == xioctl (fd, VIDIOC_S_FMT, &fmt)) {
		printf("%s: \n", dev_name[index]);
                errno_exit ("VIDIOC_S_FMT for ", dev_name[index]);
	}

	printf("pixelformat = %c%c%c%c (%c%c%c%c)\n",
	       (fmt.fmt.pix_mp.pixelformat >> 0) & 0xff,
	       (fmt.fmt.pix_mp.pixelformat >> 8) & 0xff,
	       (fmt.fmt.pix_mp.pixelformat >> 16) & 0xff,
	       (fmt.fmt.pix_mp.pixelformat >> 24) & 0xff,
	       (format[index] >> 0) & 0xff,
	       (format[index] >> 8) & 0xff,
	       (format[index] >> 16) & 0xff,
	       (format[index] >> 24) & 0xff);
	printf("num_planes = %d\n", fmt.fmt.pix_mp.num_planes);
	for (i=0; i<fmt.fmt.pix_mp.num_planes; i++) {
		printf("plane_fmt[%d].sizeimage = %d\n",
		       i, fmt.fmt.pix_mp.plane_fmt[i].sizeimage);
		printf("plane_fmt[%d].bytesperline = %d\n",
		       i, fmt.fmt.pix_mp.plane_fmt[i].bytesperline);
	}
        /* Note VIDIOC_S_FMT may change width and height. */
#if 0
        /* Buggy driver paranoia. */
        min = fmt.fmt.pix_mp.width * 2;
        if (fmt.fmt.pix_mp.bytesperline < min)
                fmt.fmt.pix_mp.bytesperline = min;
        min = fmt.fmt.pix_mp.bytesperline * fmt.fmt.pix_mp.height;
        if (fmt.fmt.pix_mp.sizeimage < min)
                fmt.fmt.pix_mp.sizeimage = min;
#endif
        switch (io) {
        case IO_METHOD_READ:
        case IO_METHOD_USERPTR:
                break;

        case IO_METHOD_MMAP:
                init_mmap (fd, index, buftype, N_BUFFERS);
                break;
        }
}

static void
close_device                    (int fd, int index)
{
	printf("closing the device ...");fflush(stdout);
        if (-1 == close (fd))
                errno_exit ("close for ", dev_name[index]);
	printf("done.\n");

        fd = -1;
}

double gettimeofday_sec()
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	return tv.tv_sec + tv.tv_usec * 1e-6;
}


static int
open_device                     (char *name)
{
        struct stat st;
	double t1, t2;
	int fd;

        if (-1 == stat (name, &st)) {
                fprintf (stderr, "Cannot identify '%s': %d, %s\n",
                         name, errno, strerror (errno));
                exit (EXIT_FAILURE);
        }

        if (!S_ISCHR (st.st_mode)) {
                fprintf (stderr, "%s is no device\n", name);
                exit (EXIT_FAILURE);
        }


	t1 = gettimeofday_sec();
        fd = open (name, O_RDWR /* required | O_NONBLOCK */, 0);
	t2 = gettimeofday_sec();

        if (-1 == fd) {
                fprintf (stderr, "Cannot open '%s': %d, %s\n",
                         name, errno, strerror (errno));
                exit (EXIT_FAILURE);
        }
	printf("overhead for open() = %lf\n", t2 - t1);
	return fd;
}

static void list_formats(int fd, int index, enum v4l2_buf_type buftype)
{
	int i;
	struct v4l2_fmtdesc fmt;
	fmt.index = i = 0;
	fmt.type = buftype;

	printf("List of pixel formats supported by %s ...\n", dev_name[index]);
	while(-1 != xioctl(fd, VIDIOC_ENUM_FMT, &fmt)) {
		printf("%i: %c%c%c%c (%s)\n", fmt.index,
		       fmt.pixelformat >> 0, fmt.pixelformat >> 8,
		       fmt.pixelformat >> 16, fmt.pixelformat >> 24, fmt.description);
		memset(&fmt, 0, sizeof(struct v4l2_fmtdesc));
		fmt.index = ++i;
		fmt.type = buftype;
	}
}

static void
usage                           (FILE *                 fp,
                                 int                    argc,
                                 char **                argv)
{
        fprintf (fp,
                 "Usage: %s [options]\n\n"
                 "Options:\n"
                 "-h | --help               Print this message\n"
                 "-d | --input_device name  Video device name for input [/dev/video0]\n"
                 "-D | --output_device name Video device name for output [/dev/video1]\n"
                 "-c | --input_color \n"
                 "-C | --output_color \n"
                 "-s | --input_size \n"
                 "-S | --output_size \n"
                 "-f | --input_file name    Specify a file to input\n"
                 "-F | --output_file name   Specify a file to output\n"
                 "",
                 argv[0]);
}

static const char short_options [] = "hc:C:d:D:f:F:s:S:";

static const struct option
long_options [] = {
        { "help",       no_argument,            NULL,           'h' },
        { "input_color",     required_argument,      NULL,           'c' },
        { "outout_color",     required_argument,      NULL,           'C' },
        { "input_device",     required_argument,      NULL,           'd' },
        { "outout_device",     required_argument,      NULL,           'D' },
        { "input_file",      required_argument,      NULL,           'f' },
        { "output_file",     required_argument,      NULL,           'F' },
        { "input_size",     required_argument,      NULL,           's' },
        { "outout_size",     required_argument,      NULL,           'S' },
        { 0, 0, 0, 0 }
};

struct sizes_t {
	const char *name;
	int w;
	int h;
};

static const struct sizes_t sizes[] = {
	{ "QCIF", 176,  144 },
	{ "CIF",  352,  288 },
	{ "QVGA", 320,  240 },
	{ "VGA",  640,  480 },
	{ "D1",   720,  480 },
	{ "WVGA", 800,  480 },
	{ "SVGA", 800,  600 },
	{ "XGA",  1024, 768 },
	{ "720p", 1280, 720 },
	{ "SXGA", 1280, 1024 },
	{ "1080p", 1920, 1080 },
};

static int set_size (char * arg, int * w, int * h)
{
	int nr_sizes = sizeof(sizes) / sizeof(sizes[0]);
	int i;

	if (!arg)
		return -1;

	for (i=0; i<nr_sizes; i++) {
		if (!strcasecmp (arg, sizes[i].name)) {
			*w = sizes[i].w;
			*h = sizes[i].h;
			return 0;
		}
	}

	return -1;
}

static const char * show_size (int w, int h)
{
	int nr_sizes = sizeof(sizes) / sizeof(sizes[0]);
	int i;

	for (i=0; i<nr_sizes; i++) {
		if (w == sizes[i].w && h == sizes[i].h)
			return sizes[i].name;
	}

	return "";
}

struct extensions_t {
	const char *ext;
	uint32_t fourcc;
	enum v4l2_mbus_pixelcode code;
	int n_planes;
};

static const struct extensions_t exts[] = {
	{ "RGB565",   V4L2_PIX_FMT_RGB565, V4L2_MBUS_FMT_ARGB8888_1X32, 1 },
	{ "rgb",      V4L2_PIX_FMT_RGB565, V4L2_MBUS_FMT_ARGB8888_1X32, 1 },
	{ "RGB888",   V4L2_PIX_FMT_RGB24, V4L2_MBUS_FMT_ARGB8888_1X32, 1 },
	{ "888",      V4L2_PIX_FMT_RGB24, V4L2_MBUS_FMT_ARGB8888_1X32, 1 },
	{ "BGR888",   V4L2_PIX_FMT_BGR24, V4L2_MBUS_FMT_ARGB8888_1X32, 1 },
	{ "RGBx888",  V4L2_PIX_FMT_RGB32, V4L2_MBUS_FMT_ARGB8888_1X32, 1 },
	{ "x888",     V4L2_PIX_FMT_RGB32, V4L2_MBUS_FMT_ARGB8888_1X32, 1 },
	{ "YV12",     V4L2_PIX_FMT_YUV420M, V4L2_MBUS_FMT_AYUV8_1X32, 2 },
	{ "NV12",     V4L2_PIX_FMT_NV12M, V4L2_MBUS_FMT_AYUV8_1X32, 2 },
	{ "420",      V4L2_PIX_FMT_NV12M, V4L2_MBUS_FMT_AYUV8_1X32, 2 },
	{ "yuv",      V4L2_PIX_FMT_NV12M, V4L2_MBUS_FMT_AYUV8_1X32, 2 },
	{ "NV16",     V4L2_PIX_FMT_NV16M, V4L2_MBUS_FMT_AYUV8_1X32, 2 },
	{ "UYVY",     V4L2_PIX_FMT_UYVY, V4L2_MBUS_FMT_AYUV8_1X32, 1 },
};

static int set_colorspace (char * arg, uint32_t * fourcc, enum v4l2_mbus_pixelcode *code, int *n_planes)
{
	int nr_exts = sizeof(exts) / sizeof(exts[0]);
	int i;

	if (!arg)
		return -1;

	for (i=0; i<nr_exts; i++) {
		if (!strcasecmp (arg, exts[i].ext)) {
			*fourcc = exts[i].fourcc;
			*code = exts[i].code;
			*n_planes = exts[i].n_planes;
			return 0;
		}
	}

	return -1;
}

static const char * show_colorspace (uint32_t c)
{
	int nr_exts = sizeof(exts) / sizeof(exts[0]);
	int i;

	for (i=0; i<nr_exts; i++) {
		if (c == exts[i].fourcc)
			return exts[i].ext;
	}

	return "<Unknown colorspace>";
}

int
main                            (int                    argc,
                                 char **                argv)
{
        dev_name[0] = "/dev/video0";
        dev_name[1] = "/dev/video1";

        for (;;) {
                int index;
                int c;

                c = getopt_long (argc, argv,
                                 short_options, long_options,
                                 &index);

                if (-1 == c)
                        break;

                switch (c) {
                case 0: /* getopt_long() flag */
                        break;

                case 'h':
                        usage (stdout, argc, argv);
                        exit (EXIT_SUCCESS);

		case 'c': /* input colorspace */
			set_colorspace (optarg, &format[OUT], &code[OUT], &n_planes[OUT]);
			break;

		case 's': /* input size */
			set_size (optarg, &width[OUT], &height[OUT]);
			break;

		case 'C': /* output colorspace */
			set_colorspace (optarg, &format[CAP], &code[CAP], &n_planes[CAP]);
			break;

		case 'S': /* output size */
			set_size (optarg, &width[CAP], &height[CAP]);
			break;

                case 'd':
                        dev_name[0] = optarg;
                        break;

                case 'D':
                        dev_name[1] = optarg;
                        break;

                case 'f':
                        input_fd = open(optarg, O_RDONLY);
                        break;

                case 'F':
                        output_fd = open(optarg, O_WRONLY | O_CREAT, 0644);
                        break;

                default:
                        usage (stderr, argc, argv);
                        exit (EXIT_FAILURE);
                }
        }

        v4lout_fd = open_device (dev_name[OUT]);
        v4lcap_fd = open_device (dev_name[CAP]);


#if 1
	list_formats(v4lout_fd, OUT,
		     V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	list_formats(v4lcap_fd, CAP,
		     V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
#endif
        init_device (v4lout_fd, OUT,
		     V4L2_CAP_VIDEO_OUTPUT_MPLANE,
		     V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	/* sink pad in RPF */
	init_entity_pad (v4lsub_fd[OUT], OUT, 0, width[OUT], height[OUT], code[OUT]);
	/* source pad in RPF */
	init_entity_pad (v4lsub_fd[OUT], OUT, 1, width[OUT], height[OUT], code[CAP]);
        init_device (v4lcap_fd, CAP,
		     V4L2_CAP_VIDEO_CAPTURE_MPLANE,
		     V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	/* sink pad in WPF */
	init_entity_pad (v4lsub_fd[CAP], CAP, 0, width[CAP], height[CAP], code[CAP]);
	/* source pad in WPF */
	init_entity_pad (v4lsub_fd[CAP], CAP, 1, width[CAP], height[CAP], code[CAP]);

        queue_buffers (v4lout_fd, OUT,
		       V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        queue_buffers (v4lcap_fd, CAP,
		       V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        start_capturing (v4lout_fd, OUT,
			 V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        start_capturing (v4lcap_fd, CAP,
			 V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

        mainloop ();

        stop_capturing (v4lout_fd, OUT,
			V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        stop_capturing (v4lcap_fd, CAP,
			V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

        uninit_device (OUT);
        uninit_device (CAP);

        close_device (v4lout_fd, OUT);
        close_device (v4lcap_fd, CAP);

        exit (EXIT_SUCCESS);

        return 0;
}
