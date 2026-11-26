// Render long runs of the same retained sprite as one ordered instance draw.
// The caller has already excluded clipping, non-default compositing and opacity.
(() => {
  const minimumSprites = 256;
  const stride = 10; // words per buffered Canvas2D command
  let renderer;
  let disabled = false;

  function createRenderer() {
    const surface = document.createElement('canvas');
    const gl = surface.getContext('webgl2', {
      alpha: true, antialias: false, depth: false, stencil: false,
      premultipliedAlpha: true, preserveDrawingBuffer: true
    });
    if (!gl) return null;
    surface.addEventListener('webglcontextlost', () => { disabled = true; });

    const program = gl.createProgram();
    function attach(type, source) {
      const shader = gl.createShader(type);
      gl.shaderSource(shader, source);
      gl.compileShader(shader);
      gl.attachShader(program, shader);
      gl.deleteShader(shader);
    }
    attach(gl.VERTEX_SHADER, `#version 300 es
      layout(location=0) in vec4 axes;
      layout(location=1) in vec2 origin;
      uniform vec2 viewport;
      out vec2 uv;
      void main() {
        uv = vec2(gl_VertexID & 1, gl_VertexID >> 1);
        vec2 point = origin + axes.xy * uv.x + axes.zw * uv.y;
        gl_Position = vec4(point / viewport * vec2(2., -2.) + vec2(-1., 1.), 0., 1.);
      }`);
    attach(gl.FRAGMENT_SHADER, `#version 300 es
      precision highp float;
      in vec2 uv;
      uniform sampler2D sprite;
      out vec4 colour;
      void main() { colour = texture(sprite, uv); }`);
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) return null;
    gl.useProgram(program);
    gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
    for (const [attribute, size, offset] of [[0, 4, 0], [1, 2, 16]]) {
      gl.enableVertexAttribArray(attribute);
      gl.vertexAttribPointer(attribute, size, gl.FLOAT, false, 24, offset);
      gl.vertexAttribDivisor(attribute, 1);
    }
    gl.bindTexture(gl.TEXTURE_2D, gl.createTexture());
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    return {gl, surface, viewport: gl.getUniformLocation(program, 'viewport'),
      instances: new Float32Array(0), image: null};
  }

  Module.cppCanvasRasterBatch = function(ctx, ints, floats, start, remaining) {
    if (disabled || remaining < minimumSprites || ctx.canvas.id !== 'canvas') return -remaining;
    const key = ints[start + 9];
    let sprites = 0, consumed = 0;
    for (let i = 0; i < remaining; i++) {
      const p = start + i * stride;
      if (ints[p] === 7) continue; // absolute transform
      if (ints[p] !== 56 || ints[p + 9] !== key || floats[p + 3] <= 0 || floats[p + 4] <= 0) break;
      sprites++;
      consumed = i + 1;
    }
    const fallback = -Math.max(1, consumed);
    if (sprites < minimumSprites) return fallback;
    const image = Module.cppCanvasImages.get(key);
    const {width, height} = ctx.canvas;
    if (!image.cppCanvasClearBorder || width * height > 4194304 ||
        !width || !height || width > 4096 || height > 4096) return fallback;

    try {
      renderer ||= createRenderer();
      if (!renderer) { disabled = true; return fallback; }
      const {gl, surface} = renderer;
      if (surface.width !== width || surface.height !== height) {
        surface.width = width;
        surface.height = height;
        if (gl.drawingBufferWidth !== width || gl.drawingBufferHeight !== height) {
          disabled = true;
          return fallback;
        }
      }
      gl.viewport(0, 0, width, height);
      gl.uniform2f(renderer.viewport, width, height);
      if (renderer.image !== image) {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, image);
        if (gl.getError() !== gl.NO_ERROR) { disabled = true; return fallback; }
        renderer.image = image;
      }
      if (renderer.instances.length < sprites * 6) {
        renderer.instances = new Float32Array(6 * 2 ** Math.ceil(Math.log2(sprites)));
      }
      const instances = renderer.instances;
      const original = ctx.getTransform();
      let {a, b, c, d, e, f} = original;
      let offset = 0;
      for (let i = 0; i < consumed; i++) {
        const p = start + i * stride;
        if (ints[p] === 7) {
          a = floats[p+1]; b = floats[p+2]; c = floats[p+3];
          d = floats[p+4]; e = floats[p+5]; f = floats[p+6];
          continue;
        }
        const x = floats[p+1], y = floats[p+2], w = floats[p+3], h = floats[p+4];
        instances[offset++] = a*w;
        instances[offset++] = b*w;
        instances[offset++] = c*h;
        instances[offset++] = d*h;
        instances[offset++] = a*x + c*y + e;
        instances[offset++] = b*x + d*y + f;
      }
      gl.bufferData(gl.ARRAY_BUFFER, instances, gl.STREAM_DRAW, 0, offset);
      gl.clear(gl.COLOR_BUFFER_BIT);
      gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, sprites);
      if (gl.isContextLost()) { disabled = true; return fallback; }
      ctx.resetTransform();
      try {
        ctx.drawImage(surface, 0, 0);
      } catch (error) {
        ctx.setTransform(original);
        throw error;
      }
      ctx.setTransform(a, b, c, d, e, f);
      return consumed;
    } catch {
      disabled = true;
      return fallback;
    }
  };
})();
