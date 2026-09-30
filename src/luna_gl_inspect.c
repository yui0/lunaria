/* SPDX-License-Identifier: MPL-2.0 */
/* Diagnostic only: create the LUNARIA_UNIFORM_DUMP marker to capture four
 * frames. Remove/recreate it to capture again. Never modifies guest data. */
#include "luna_gl_inspect.h"
#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct block_sample { unsigned char *bytes; GLint length; };
static void inspect_draw_state(GLuint program, GLint fbo)
{
    GLint viewport[4], scissor[4], depth, src, dst, draw, read, ref, func;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissor);
    glGetIntegerv(GL_DEPTH_FUNC, &depth);
    glGetIntegerv(GL_BLEND_SRC_RGB, &src);
    glGetIntegerv(GL_BLEND_DST_RGB, &dst);
    glGetIntegerv(GL_DRAW_BUFFER0, &draw);
    glGetIntegerv(GL_READ_BUFFER, &read);
    glGetIntegerv(GL_STENCIL_REF, &ref);
    glGetIntegerv(GL_STENCIL_FUNC, &func);
    GLboolean color[4], write;
    glGetBooleanv(GL_COLOR_WRITEMASK, color);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &write);
    fprintf(stderr, "[gl-state] p=%u vp=%d,%d,%d,%d scissor=%d:%d,%d,%d,%d depth=%d:%x:%d blend=%d:%x,%x stencil=%d:%x:%d cull=%d discard=%d color=%d%d%d%d draw=%x read=%x\n",
        program, viewport[0],viewport[1],viewport[2],viewport[3],
        glIsEnabled(GL_SCISSOR_TEST),scissor[0],scissor[1],scissor[2],scissor[3],
        glIsEnabled(GL_DEPTH_TEST),depth,write,glIsEnabled(GL_BLEND),src,dst,
        glIsEnabled(GL_STENCIL_TEST),func,ref,glIsEnabled(GL_CULL_FACE),
        glIsEnabled(GL_RASTERIZER_DISCARD),color[0],color[1],color[2],color[3],draw,read);
    if (fbo) return;
    GLuint shaders[8]; GLsizei count = 0;
    glGetAttachedShaders(program, 8, &count, shaders);
    for (GLsizei i = 0; i < count; ++i) {
        GLint length = 0;
        glGetShaderiv(shaders[i], GL_SHADER_SOURCE_LENGTH, &length);
        if (length <= 1 || length > (1 << 20)) continue;
        char *text = malloc((size_t)length);
        if (!text) continue;
        GLsizei used = 0;
        glGetShaderSource(shaders[i], length, &used, text);
        char path[160];
        snprintf(path, sizeof path, "/tmp/lunaria-gl-%ld-p%u-s%u.glsl",
                 (long)getpid(), program, shaders[i]);
        FILE *out = fopen(path, "wb");
        if (out) { fwrite(text, 1, (size_t)used, out); fclose(out); }
        free(text);
    }
}
static void inspect_attributes(GLuint program)
{
    GLint count = 0, previous = 0;
    glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &count);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous);
    for (GLuint a = 0; a < (GLuint)count && a < 32; ++a) {
        char name[256]; GLint size; GLenum type;
        glGetActiveAttrib(program, a, sizeof name, NULL, &size, &type, name);
        GLint loc = glGetAttribLocation(program, name);
        if (loc < 0) continue;
        GLint enabled, buffer, stride, components, storage, normalized;
        void *offset = NULL;
        glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
        glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
        glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
        glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_SIZE, &components);
        glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_TYPE, &storage);
        glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &normalized);
        glGetVertexAttribPointerv(loc, GL_VERTEX_ATTRIB_ARRAY_POINTER, &offset);
        fprintf(stderr, "[gl-attrib] p=%u %s loc=%d enabled=%d buffer=%d stride=%d components=%d type=%x norm=%d offset=%llu",
            program,name,loc,enabled,buffer,stride,components,storage,normalized,(unsigned long long)(uintptr_t)offset);
        if (!enabled) {
            GLfloat current[4] = {0};
            glGetVertexAttribfv(loc, GL_CURRENT_VERTEX_ATTRIB, current);
            fprintf(stderr, " current=%g,%g,%g,%g", current[0],current[1],current[2],current[3]);
        }
        if (enabled && buffer) {
            GLint64 length = 0; GLint mapped = 0;
            glBindBuffer(GL_ARRAY_BUFFER, buffer);
            glGetBufferParameteri64v(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &length);
            glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_MAPPED, &mapped);
            if (!mapped && (uint64_t)length > (uintptr_t)offset + 32) {
                const unsigned char *data = glMapBufferRange(GL_ARRAY_BUFFER, (uintptr_t)offset, 32, GL_MAP_READ_BIT);
                if (data) {
                    fprintf(stderr, " data=");
                    for (int i = 0; i < 32; ++i) fprintf(stderr, "%02x", data[i]);
                    glUnmapBuffer(GL_ARRAY_BUFFER);
                }
            }
        }
        fprintf(stderr, "\n");
    }
    glBindBuffer(GL_ARRAY_BUFFER, previous);
}
/* Sample on the owning GL thread, with an independent sampler and target.
 * Compressed textures cannot be attached directly to a colour framebuffer. */
static void inspect_texture(GLuint owner, const char *name, GLint unit)
{
    static GLuint sampled[1024]; static int nsampled;
    GLint active, texture, sampler, filter, base, maxlevel;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0 + unit);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &base);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maxlevel);
    if (sampler) glGetSamplerParameteriv(sampler, GL_TEXTURE_MIN_FILTER, &filter);
    typedef void (*level_fn)(GLenum, GLint, GLenum, GLint *);
    level_fn level = (level_fn)eglGetProcAddress("glGetTexLevelParameteriv");
    GLint w = 0, h = 0, format = 0;
    if (level) {
        level(GL_TEXTURE_2D, base, 0x1000 /* WIDTH */, &w);
        level(GL_TEXTURE_2D, base, 0x1001 /* HEIGHT */, &h);
        level(GL_TEXTURE_2D, base, 0x1003 /* INTERNAL_FORMAT */, &format);
    }
    fprintf(stderr, "[gl-texture] p=%u %s unit=%d tex=%d sampler=%d filter=%x base=%d max=%d %dx%d format=%x\n",
            owner, name, unit, texture, sampler, filter, base, maxlevel, w, h, format);
    glActiveTexture(active);
    if (!texture || !w || !h || nsampled == 1024) return;
    for (int i = 0; i < nsampled; ++i) if (sampled[i] == (GLuint)texture) return;
    sampled[nsampled++] = texture;

    GLint program, vao, readfb, drawfb, view[4], tex0, samp0, pack;
    GLint pack_values[4]; GLboolean mask[4];
    const GLenum pack_names[] = {GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS};
    const GLenum caps[] = {GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_STENCIL_TEST,
        GL_SCISSOR_TEST, GL_RASTERIZER_DISCARD, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE};
    GLboolean enabled[sizeof caps / sizeof caps[0]];
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readfb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawfb);
    glGetIntegerv(GL_VIEWPORT, view); glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    glGetIntegerv(GL_SAMPLER_BINDING, &samp0);
    for (unsigned i = 0; i < sizeof caps / sizeof caps[0]; ++i) {
        enabled[i] = glIsEnabled(caps[i]); glDisable(caps[i]);
    }
    for (unsigned i = 0; i < 4; ++i) {
        glGetIntegerv(pack_names[i], &pack_values[i]); glPixelStorei(pack_names[i], i ? 0 : 1);
    }
    static GLuint probe, target, fb, array, nearest;
    if (!probe) {
        const char *vs = "#version 300 es\n out vec2 uv;void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);uv=p;gl_Position=vec4(p*2.-1.,0.,1.);}";
        const char *fs = "#version 300 es\n precision highp float;in vec2 uv;uniform sampler2D image;out vec4 color;void main(){color=textureLod(image,uv,0.);}";
        GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(v, 1, &vs, NULL); glCompileShader(v);
        glShaderSource(f, 1, &fs, NULL); glCompileShader(f);
        probe = glCreateProgram(); glAttachShader(probe, v); glAttachShader(probe, f); glLinkProgram(probe);
        glDeleteShader(v); glDeleteShader(f);
        glGenVertexArrays(1, &array); glGenFramebuffers(1, &fb);
        glGenTextures(1, &target); glBindTexture(GL_TEXTURE_2D, target);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 32, 32);
        glGenSamplers(1, &nearest);
        glSamplerParameteri(nearest, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glSamplerParameteri(nearest, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glSamplerParameteri(nearest, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(nearest, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    glBindVertexArray(array); glUseProgram(probe);
    glUniform1i(glGetUniformLocation(probe, "image"), 0);
    glBindTexture(GL_TEXTURE_2D, texture); glBindSampler(0, nearest);
    glViewport(0, 0, 32, 32); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    unsigned char pixels[32 * 32 * 4] = {0};
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glReadPixels(0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    unsigned lo[4] = {255,255,255,255}, hi[4] = {0}, sum[4] = {0};
    for (unsigned i = 0; i < sizeof pixels; ++i) {
        unsigned c = i % 4, v = pixels[i];
        if (v < lo[c]) lo[c] = v;
        if (v > hi[c]) hi[c] = v;
        sum[c] += v;
    }
    fprintf(stderr, "[gl-texture] sampled tex=%d min=%u,%u,%u,%u max=%u,%u,%u,%u avg=%u,%u,%u,%u\n",
        texture, lo[0],lo[1],lo[2],lo[3],hi[0],hi[1],hi[2],hi[3],sum[0]/1024,sum[1]/1024,sum[2]/1024,sum[3]/1024);
    const char *dir = getenv("LUNARIA_DUMP_DIR");
    if (dir) {
        char path[4096]; snprintf(path, sizeof path, "%s/texture_%u.ppm", dir, texture);
        FILE *out = fopen(path, "wb");
        if (out) {
            fprintf(out, "P6\n32 32\n255\n");
            for (int y = 31; y >= 0; --y) for (int x = 0; x < 32; ++x) fwrite(pixels + (y * 32 + x) * 4, 1, 3, out);
            fclose(out);
        }
    }
    /* Compare with the guest's sampler: a readable base level can still be
     * incomplete under the selected mipmap filter. */
    glBindSampler(0, sampler);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadPixels(0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    memset(sum, 0, sizeof sum);
    for (unsigned i = 0; i < sizeof pixels; ++i) sum[i % 4] += pixels[i];
    fprintf(stderr, "[gl-texture] guest-sampler tex=%d avg=%u,%u,%u,%u\n",
            texture, sum[0]/1024,sum[1]/1024,sum[2]/1024,sum[3]/1024);
    glBindTexture(GL_TEXTURE_2D, tex0); glBindSampler(0, samp0); glActiveTexture(active);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pack);
    for (unsigned i = 0; i < 4; ++i) glPixelStorei(pack_names[i], pack_values[i]);
    glUseProgram(program); glBindVertexArray(vao);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readfb); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawfb);
    glViewport(view[0], view[1], view[2], view[3]); glColorMask(mask[0],mask[1],mask[2],mask[3]);
    for (unsigned i = 0; i < sizeof caps / sizeof caps[0]; ++i) if (enabled[i]) glEnable(caps[i]);
}
void luna_gl_inspect_draw(uint64_t frame)
{
    static int init, requested, seen_count;
    static const char *marker;
    static uint64_t checked = UINT64_MAX, start, seen_frame;
    static GLuint seen[128];
    if (!init) { init = 1; marker = getenv("LUNARIA_UNIFORM_DUMP"); }
    if (!marker || !*marker) return;
    if (checked != frame) {
        checked = frame;
        int exists = access(marker, F_OK) == 0;
        if (exists && !requested) start = frame;
        requested = exists;
    }
    if (!requested || frame - start >= 4) return;
    if (seen_frame != frame) { seen_frame = frame; seen_count = 0; }
    GLint program = 0, n = 0, nb = 0, generic = 0, fbo = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    if (!program) return;
    for (int i = 0; i < seen_count; ++i) if (seen[i] == (GLuint)program) return;
    if (seen_count == 128) return;
    seen[seen_count++] = (GLuint)program;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &n);
    glGetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &nb);
    glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &generic);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    fprintf(stderr, "[gl-inspect] frame=%llu program=%d fbo=%d uniforms=%d blocks=%d\n",
            (unsigned long long)frame, program, fbo, n, nb);
    inspect_draw_state((GLuint)program, fbo);
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (extensions && strstr(extensions, "GL_EXT_sRGB_write_control"))
        fprintf(stderr, "[gl-inspect] framebuffer-srgb=%d\n", glIsEnabled(0x8db9));
    if (fbo) {
        GLint count = 0;
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &count);
        for (int i = 0; i < count && i < 8; ++i) {
            GLint attachment = 0, type = 0, object = 0, encoding = 0;
            glGetIntegerv(GL_DRAW_BUFFER0 + i, &attachment);
            if (attachment == GL_NONE) continue;
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment,
                GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
            if (type == GL_NONE) continue;
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment,
                GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &object);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment,
                GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
            fprintf(stderr, "[gl-inspect] draw-buffer=%d attachment=%x type=%x object=%d encoding=%x\n",
                i, attachment, type, object, encoding);
        }
    }
    inspect_attributes(program);
    if (nb > 32) nb = 32;
    struct block_sample blocks[32] = {{0}};
    for (int b = 0; b < nb; ++b) {
        GLint binding = 0, bytes = 0, buffer = 0, total = 0, mapped = 0;
        GLint64 offset = 0, range = 0;
        glGetActiveUniformBlockiv(program, b, GL_UNIFORM_BLOCK_BINDING, &binding);
        glGetActiveUniformBlockiv(program, b, GL_UNIFORM_BLOCK_DATA_SIZE, &bytes);
        glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, binding, &buffer);
        glGetInteger64i_v(GL_UNIFORM_BUFFER_START, binding, &offset);
        glGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, binding, &range);
        fprintf(stderr, "[gl-inspect] block=%d binding=%d buffer=%d offset=%lld range=%lld bytes=%d\n",
                b, binding, buffer, (long long)offset, (long long)range, bytes);
        if (!buffer || bytes <= 0 || bytes > 65536) continue;
        glBindBuffer(GL_UNIFORM_BUFFER, buffer);
        glGetBufferParameteriv(GL_UNIFORM_BUFFER, GL_BUFFER_SIZE, &total);
        glGetBufferParameteriv(GL_UNIFORM_BUFFER, GL_BUFFER_MAPPED, &mapped);
        if (mapped || offset < 0 || offset + bytes > total || (range && range < bytes)) continue;
        void *ptr = glMapBufferRange(GL_UNIFORM_BUFFER, offset, bytes, GL_MAP_READ_BIT);
        if (!ptr) continue;
        blocks[b].bytes = malloc((size_t)bytes);
        if (blocks[b].bytes) { memcpy(blocks[b].bytes, ptr, (size_t)bytes); blocks[b].length = bytes; }
        glUnmapBuffer(GL_UNIFORM_BUFFER);
    }
    glBindBuffer(GL_UNIFORM_BUFFER, generic);
    for (GLuint u = 0; u < (GLuint)n && u < 2048; ++u) {
        char name[256] = {0}; GLsizei length = 0; GLint size = 0, block = -1, offset = -1;
        GLenum type = 0;
        glGetActiveUniform(program, u, sizeof name, &length, &size, &type, name);
        glGetActiveUniformsiv(program, 1, &u, GL_UNIFORM_BLOCK_INDEX, &block);
        GLfloat values[16] = {0}; GLint integers[16] = {0};
        int valid = 0, integral = 0;
        if (block >= 0 && block < nb) {
            glGetActiveUniformsiv(program, 1, &u, GL_UNIFORM_OFFSET, &offset);
            if (blocks[block].bytes && offset >= 0 && offset < blocks[block].length) {
                size_t bytes = (size_t)(blocks[block].length - offset);
                if (bytes > sizeof values) bytes = sizeof values;
                memcpy(values, blocks[block].bytes + offset, bytes); valid = 1;
            }
        } else {
            GLint location = glGetUniformLocation(program, name);
            if (location >= 0) {
                integral = !(type == GL_FLOAT || (type >= GL_FLOAT_VEC2 && type <= GL_FLOAT_VEC4) ||
                             (type >= GL_FLOAT_MAT2 && type <= GL_FLOAT_MAT4) ||
                             (type >= GL_FLOAT_MAT2x3 && type <= GL_FLOAT_MAT4x3));
                if (integral) glGetUniformiv(program, location, integers);
                else glGetUniformfv(program, location, values);
                valid = 1;
            }
        }
        fprintf(stderr, "[gl-inspect] p=%d %s type=%x size=%d block=%d offset=%d valid=%d ",
                program, name, type, size, block, offset, valid);
        if (integral) fprintf(stderr, "i=%d,%d,%d,%d\n", integers[0], integers[1], integers[2], integers[3]);
        else fprintf(stderr, "f=%.7g,%.7g,%.7g,%.7g\n", values[0], values[1], values[2], values[3]);
        if (valid && type == GL_SAMPLER_2D) inspect_texture(program, name, integers[0]);
    }
    for (int b = 0; b < nb; ++b) free(blocks[b].bytes);
}
