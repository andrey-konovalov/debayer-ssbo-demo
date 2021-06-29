/* SPDX-License-Identifier: LGPL-3.0 */
/*
 * Command line demo application which uses compute shader in a window-less
 * EGL + GLES 3.1 context to demosaic 8-bit raw bayer image.
 *
 * Copyright (C) 2021, Linaro
 *
 * The method to run a headless compute shader is taken from the blog post
 * by Eduardo Lima Mitev <elima@igalia.com> :
 * https://blogs.igalia.com/elima/2016/10/06/example-run-an-opengl-es-compute-shader-on-a-drm-render-node/
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <assert.h>
#include <fcntl.h>
#include <gbm.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RENDER_NODE_FNAME "/dev/dri/renderD128"

#define SHADER_FNAME "./debayer.comp"

enum {
	bo_in,
	bo_out,
	bo_num
};

long read_input_file(const char *fname, char **data, const char *type)
{
	FILE *fp;
	long size;
	char *p_data;

	fp = fopen(fname, type);
	if (fp == NULL)
		return 0;
	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (size <= 0)
		goto err_seek;

	p_data = malloc(size);
	if (p_data == NULL)
		goto err_seek;

	if (fread(p_data, 1, size, fp) != size)
		goto err_fread;

	fclose(fp);
	*data = p_data;
	return size;

err_fread:
	free(p_data);
err_seek:
	fclose(fp);
	return -1;
}


long read_input_bin_file(const char *fname, char **data)
{
	return read_input_file(fname, data, "rb");
}

long read_input_text_file(const char *fname, char **data)
{
	return read_input_file(fname, data, "r");
}

long write_output_file(const char *fname, const char *p_data, long data_size)
{
	FILE *fp;

	fp = fopen(fname, "wb");
	if (fp == NULL)
		return 0;
	if (fwrite(p_data, 1, data_size, fp) != data_size) {
		fclose(fp);
		return -1;
	}

	fclose(fp);
	return data_size;
}

struct converter {
	/* EGL realted stuff */
	int fd;		/* render node fd */
	struct gbm_device *gbm;
	EGLDisplay egl_dpy;
	EGLContext core_ctx;

	/* buffers related stuff */
	GLuint bos[bo_num];
	uint8_t * p_in;
	const uint8_t * p_out;

	/* shader */
	GLuint shader_program;
	GLuint compute_shader;
	const char * shader_fname;
};

int init_egl(struct converter * conv, const char * render_node)
{
	const char *egl_extension_st;
	static const EGLint config_attribs[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_NONE
	};
	static const EGLint attribs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3,
		EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE
	};
	EGLConfig cfg;
	EGLint count;
	EGLint major, minor;

	conv->fd = open (render_node, O_RDWR);
	if (conv->fd < 0) {
		perror("init_opengl: ");
		return -1;
	}

	conv->gbm = gbm_create_device(conv->fd);
	if (conv->gbm == NULL) {
		printf("init_opengl: failed to create GBM device\n");
		goto err_gbm;
	}

	/* setup EGL from the GBM device */
	conv->egl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA,
					      conv->gbm, NULL);
	if (conv->egl_dpy == NULL) {
		printf("init_opengl: eglGetPlatformDisplay() failed\n");
		goto err_egl_dpy;
	}

	/* initialize an EGL display connection */
	if (eglInitialize(conv->egl_dpy, &major, &minor) != EGL_TRUE) {
		printf("init_opengl: eglInitialize() failed\n");
		goto err_egl_ctx;
	} else {
		printf("EGL version: %d.%d\n", major, minor);
	}

	egl_extension_st = eglQueryString (conv->egl_dpy, EGL_EXTENSIONS);
	if (strstr (egl_extension_st, "EGL_KHR_create_context") == NULL ||
	    strstr (egl_extension_st, "EGL_KHR_surfaceless_context") == NULL) {
		printf("init_opengl: EGL_KHR_create_context or EGL_KHR_surfaceless_context not supported\n");
		goto err_egl_ctx;
	}

	if (!eglChooseConfig(conv->egl_dpy, config_attribs, NULL, 0, &count)) {
                printf("init_opengl: eglChooseConfig(&cfg == NULL) failed\n");
        }
        printf("eglChooseConfig(): %d matching configs available\n", count);

	/*
	 * Get the first EGL frame buffer configuration that matches the
	 * specified attributes - we request GL ES 3.x.
	 */
	if (!eglChooseConfig(conv->egl_dpy, config_attribs, &cfg, 1, &count)) {
		printf("init_opengl: eglChooseConfig() failed: %d\n",
		       eglGetError());
		goto err_egl_ctx;
	}

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		printf("init_opengl: eglBindAPI() failed: %d\n", eglGetError());
		goto err_egl_ctx;
	}

	conv->core_ctx = eglCreateContext(conv->egl_dpy, cfg, EGL_NO_CONTEXT,
					  attribs);
	if (conv->core_ctx == EGL_NO_CONTEXT) {
		printf("init_opengl: eglCreateContext() failed\n");
		goto err_egl_ctx;
	}

	/*
	 * eglMakeCurrent() binds context to the current rendering thread.
	 * We don't need neither draw nor read surfaces hence EGL_NO_SURFACE's.
	 */
	if (!eglMakeCurrent(conv->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
			    conv->core_ctx)) {
		printf("init_opengl: eglMakeCurrent() failed: %d\n",
		       eglGetError());
		goto err_egl_make_current;
	}

	glGetIntegerv(GL_MAJOR_VERSION, &major);
	glGetIntegerv(GL_MINOR_VERSION, &minor);
	printf("*** EGL version: %d.%d\n", major, minor);

	return 0;

err_egl_make_current:
	eglDestroyContext(conv->egl_dpy, conv->core_ctx);
err_egl_ctx:
	eglTerminate(conv->egl_dpy);
err_egl_dpy:
	gbm_device_destroy(conv->gbm);
err_gbm:
	close(conv->fd);
	return -1;
}

void deinit_egl(struct converter *conv)
{
	eglDestroyContext(conv->egl_dpy, conv->core_ctx);
	eglTerminate(conv->egl_dpy);
	gbm_device_destroy(conv->gbm);
	close(conv->fd);
}

int init_shader(struct converter *conv)
{
	GLenum err;
	GLint param;
	char *compile_log;

	int shader_cnt;
	char *shader_src;

	shader_cnt = read_input_text_file(conv->shader_fname, &shader_src);
	if (shader_cnt <= 0) {
		printf("Fail to read the shader source from file \"%s\"\n",
		       conv->shader_fname);
		return GL_TRUE; /* GL_NO_ERROR == GL_FALSE */
	}
	printf("%d bytes read from \"%s\"\n", shader_cnt, conv->shader_fname);

	conv->compute_shader = glCreateShader(GL_COMPUTE_SHADER);
	if(!conv->compute_shader) {
		free(shader_src);
		return glGetError();
	}

	glShaderSource(conv->compute_shader, 1, &shader_src, &shader_cnt);
	/*
	 * The shader source has been copied into the shader object, so
	 * shader_src[] contents is no longer needed.
	 */
	free(shader_src);

	if ((err = glGetError()) != GL_NO_ERROR)
		return err;

	glCompileShader(conv->compute_shader);
	glGetShaderiv(conv->compute_shader, GL_COMPILE_STATUS, &param);
	if (param != GL_TRUE) {
		glGetShaderiv(conv->compute_shader, GL_INFO_LOG_LENGTH, &param);
		printf("glCompileShader() failed");
		compile_log = malloc(param);
		if (compile_log == NULL) {
			printf(", no log is available\n");
			return GL_TRUE; /* GL_NO_ERROR == GL_FALSE */
		}
		glGetShaderInfoLog(conv->compute_shader, param, NULL,
				   compile_log);
		if (glGetError() == GL_NO_ERROR)
			printf("glCompileShader failed:\n"
			       "--- log ---\n%s\n--- log ---\n", compile_log);
		free(compile_log);
		return GL_TRUE;
	}

	conv->shader_program = glCreateProgram();
	if(!conv->shader_program) {
		err = glGetError();
		goto err_del_shader;
	}

	glAttachShader(conv->shader_program, conv->compute_shader);
	if ((err = glGetError()) != GL_NO_ERROR)
		goto err_del_program;

	glLinkProgram(conv->shader_program);
	if ((err = glGetError()) != GL_NO_ERROR)
		goto err_del_program;

	glDeleteShader(conv->compute_shader);
	return 0;

err_del_program:
	glDeleteProgram(conv->shader_program);
err_del_shader:
	glDeleteShader(conv->compute_shader);
	return err;
}

int use_shader(GLuint shader_program)
{
	glUseProgram(shader_program);
	return (glGetError() != GL_NO_ERROR);
}

void free_shader(struct converter * conv)
{
	/* glDeleteShader() had been called at this point */
	glDeleteProgram(conv->shader_program);
}

#define LSIZE_X 32
#define LSIZE_Y 8
/* number of workgroups by X and Y */
#define WG_NUM_X (1920 / LSIZE_X)
#define WG_NUM_Y (1080 / LSIZE_Y)

#define USAGE \
	"Usage: %s [-h] -s XxY -f <format> <inputfile> <outputfile>\n" \
	"-f <order>   Specify input file format\n" \
	"-s XxY       Specify input image size (e.g. 640x480)\n" \
	"-h           Shows this help\n"

static int parse_bayer_order(const char *p, int *bo)
{
	return -1; /* not implemented yet */
}

int main(int argc, char* argv[])
{
	struct converter cvt;
	int b_ord = -1;
	char *p_data_in; /* copy of the data from the input file */
	long data_in_size, data_out_size;
	int i;

	cvt.shader_fname = SHADER_FNAME;

	/* Process cmd line options */
	for (;;) {
		int c = getopt(argc, argv, "f:h");
		if (c == -1) break;
		switch (c) {
		case 'f':
			if (parse_bayer_order(optarg, &b_ord) < 0) {
				printf("bad bayer order\n");
				return -1;;
			}
			break;
		case 'h':
			printf(USAGE, argv[0]);
			return 0;
		}
	}
	if (argc - optind != 2) {
		printf("Give input and output files\n");
		return -1;
	}

	/* Read the file to process into memory */
	data_in_size = read_input_bin_file(argv[optind], &p_data_in);
	if (data_in_size <= 0) {
		printf("Failed to read input file \"%s\"\n", argv[optind]);
		return -1;
	}
	data_out_size = 4 * data_in_size;

	/* Initialize OpenGL stuff */
	if (init_egl(&cvt, RENDER_NODE_FNAME) != 0) {
		printf("EGL initialization failed\n");
		exit(EXIT_FAILURE);
	}

	if (init_shader(&cvt) != 0) {
		printf("Shader creation failed\n");
		exit(EXIT_FAILURE);
	}

	/* Do the things here... */
	GLenum err;
	void *data;
	glGenBuffers(bo_num, cvt.bos);

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, cvt.bos[bo_in]);
	glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizei)data_in_size,
		     p_data_in, GL_STREAM_DRAW);
	err = glGetError();
	if (err != GL_NO_ERROR) {
		printf("glBufferData(in, size=%ld) error 0x%04X\n",
		       data_in_size, err);
		goto err_buffs;
	}
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, cvt.bos[bo_out]);
	glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizei)data_out_size,
		     NULL, GL_STREAM_READ);
	err = glGetError();
	if (err != GL_NO_ERROR) {
		printf("glBufferData(out, size=%ld) error 0x%04X\n",
		       data_out_size, err);
		goto err_buffs;
	}
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, cvt.bos[bo_in]);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, cvt.bos[bo_out]);
	GLsync sync;
	for (i=0; i < 1; i++) {
		if (use_shader(cvt.shader_program) != 0) {
			printf("use_shader() failed \n");
			break;
		}
		glDispatchCompute(WG_NUM_X, WG_NUM_Y, 1);

		err = glGetError();
		if (err != GL_NO_ERROR) {
			printf("glDispatchCompute() error 0x%04X\n", err);
			break;
		}

		glMemoryBarrier(GL_ALL_BARRIER_BITS);

		sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	}

	glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT,
			 100*1000 /* 100mS */);

	/* Write the output buffer to the file */
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, cvt.bos[bo_out]);
	data = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, data_out_size,
				GL_MAP_READ_BIT);
	if (data == NULL) {
		printf("glMapBufferRange(out) error 0x%04X\n",
		       glGetError());
	} else {
		if (write_output_file(argv[optind+1], data, data_out_size)
		    == data_out_size)
			printf("%s: %ld bytes written\n", argv[optind+1],
			       data_out_size);
		glUnmapBuffer(GL_COPY_WRITE_BUFFER);
	}

	/* Cleanup and exit */
err_buffs:
	/* glDeleteTextures(1, &cvt.tbo_tex); */
	glDeleteBuffers(bo_num, cvt.bos);
	free_shader(&cvt);
	deinit_egl(&cvt);
	free(p_data_in);
	return err;
}
