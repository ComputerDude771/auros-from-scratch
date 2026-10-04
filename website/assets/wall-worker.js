/* Draws a wallpaper off the page's main thread. See wall.js. */
importScripts("wall.js");
self.onmessage = function (e) {
  var d = e.data;
  var r = self.AurWall.render(d.w, d.h, d.p, d.seed);
  self.postMessage({ id: d.id, buf: r.pixels.buffer, meta: r.meta }, [r.pixels.buffer]);
};
