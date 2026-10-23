/* The six AurOS themes, as data.
 *
 * Every value below is copied from themes/<id>.theme in the AurOS
 * repository. Sandstone, Moss, Synthwave, Ember and Slate inherit from
 * Nocturne there and state only what differs; here the inheritance has
 * been resolved so each entry is complete. Nothing is invented: if a
 * key is not in a theme file it is not in this one.
 */
(function (root) {
  "use strict";

  var nocturne = {
    id: "nocturne", name: "Nocturne", variant: "dark", glyph: "◆",
    description: "Deep atmospheric blue-black with an aurora-teal glow.",
    bg: "#0E1013", bg_alt: "#14171B", surface: "#191D21", surface_hi: "#23272D",
    overlay: "#6E7680", muted: "#6A7178", subtle: "#9BA3AB", fg: "#D9DDE1", fg_hi: "#F4F6F8",
    accent: "#C89B4A", accent_alt: "#7C8B9B", accent_warm: "#A97B3E",
    ok: "#8FB573", warn: "#C89B4A", err: "#C96A5E", info: "#7C8B9B",
    ansi: ["#191D21", "#C96A5E", "#8FB573", "#C89B4A", "#7C8B9B", "#A08099", "#6F9A96", "#D9DDE1",
           "#6A7178", "#DE8578", "#A8CB8C", "#DDB365", "#95A4B4", "#B99AB2", "#8BB4B0", "#F4F6F8"],
    radius: 14, radius_sm: 8, gap: 12, bar_height: 38, animation_ms: 170,
    wall_style: "letterpress",
    wall_c1: "#0C0E11", wall_c2: "#15181C", wall_c3: "#C89B4A", wall_c4: "#1B1F24",
    wall_intensity: 0.40, wall_grain: 0.05, wall_vignette: 0.45
  };

  function inherit(over) {
    var t = {}, k;
    for (k in nocturne) t[k] = nocturne[k];
    for (k in over) t[k] = over[k];
    return t;
  }

  var THEMES = {
    nocturne: nocturne,

    sandstone: inherit({
      id: "sandstone", name: "Sandstone", variant: "light",
      description: "Warm paper and ink with a terracotta accent.",
      bg: "#F4F0E8", bg_alt: "#EDE7DC", surface: "#FFFFFF", surface_hi: "#F0E9DC",
      overlay: "#877D6D", muted: "#8F8068", subtle: "#6F6555", fg: "#38332B", fg_hi: "#1C1916",
      accent: "#C2683D", accent_alt: "#4E7A6B", accent_warm: "#D9A441",
      ok: "#4E7A6B", warn: "#C4892B", err: "#B4453C", info: "#3F6E9E",
      ansi: ["#38332B", "#B4453C", "#4E7A6B", "#C4892B", "#3F6E9E", "#8A5C8F", "#3E8A87", "#F4F0E8",
             "#6F6555", "#C9594E", "#5F9382", "#D9A441", "#5387B8", "#A173A6", "#4FA5A1", "#FFFFFF"],
      radius: 12,
      wall_style: "mesh",
      wall_c1: "#F4F0E8", wall_c2: "#E3D8C6", wall_c3: "#C2683D", wall_c4: "#4E7A6B",
      wall_intensity: 0.32, wall_grain: 0.045, wall_vignette: 0.22
    }),

    moss: inherit({
      id: "moss", name: "Moss", variant: "dark", glyph: "✦",
      description: "A quiet forest floor — desaturated greens and warm stone.",
      bg: "#0E1310", bg_alt: "#141A16", surface: "#1A221C", surface_hi: "#232D25",
      overlay: "#627D69", muted: "#677969", subtle: "#8FA292", fg: "#D3DED4", fg_hi: "#F0F6F0",
      accent: "#8FBF7F", accent_alt: "#C9A86C", accent_warm: "#E0B88A",
      ok: "#8FBF7F", warn: "#E0B88A", err: "#D2807C", info: "#87AFC0",
      ansi: ["#1A221C", "#D2807C", "#8FBF7F", "#C9A86C", "#87AFC0", "#B29BC4", "#7FC0B4", "#D3DED4",
             "#5A6A5C", "#E59A96", "#A9D69A", "#E0C58A", "#A3C7D6", "#C9B5D9", "#9AD6C9", "#F0F6F0"],
      radius: 18, radius_sm: 10, gap: 14,
      wall_style: "mesh",
      wall_c1: "#0A0F0C", wall_c2: "#1B2C1F", wall_c3: "#8FBF7F", wall_c4: "#C9A86C",
      wall_intensity: 0.40, wall_grain: 0.06, wall_vignette: 0.50
    }),

    synthwave: inherit({
      id: "synthwave", name: "Synthwave", variant: "dark", glyph: "▲",
      description: "Neon grid, hot magenta and cyan over a violet horizon.",
      bg: "#160A26", bg_alt: "#1D0E32", surface: "#26123F", surface_hi: "#33194F",
      overlay: "#8B5AC8", muted: "#8462AA", subtle: "#A98CC9", fg: "#EBDCFA", fg_hi: "#FFFFFF",
      accent: "#FF4FD8", accent_alt: "#4FE9FF", accent_warm: "#FFC95C",
      ok: "#4FFFB0", warn: "#FFC95C", err: "#FF5C7A", info: "#4FE9FF",
      ansi: ["#26123F", "#FF5C7A", "#4FFFB0", "#FFC95C", "#7A8CFF", "#FF4FD8", "#4FE9FF", "#EBDCFA",
             "#6E4E91", "#FF8FA5", "#8CFFD0", "#FFDE9B", "#A8B4FF", "#FF93E7", "#96F3FF", "#FFFFFF"],
      radius: 6, radius_sm: 3,
      wall_style: "waves",
      wall_c1: "#0D0518", wall_c2: "#3D1160", wall_c3: "#FF4FD8", wall_c4: "#4FE9FF",
      wall_intensity: 0.78, wall_grain: 0.07, wall_vignette: 0.55
    }),

    ember: inherit({
      id: "ember", name: "Ember", variant: "dark", glyph: "✧",
      description: "Warm charcoal and banked firelight, for a screen that has been on for nine years.",
      bg: "#12100E", bg_alt: "#191613", surface: "#211D19", surface_hi: "#2C2721",
      overlay: "#7A6A59", muted: "#8A7A68", subtle: "#B0A08C", fg: "#E6DED2", fg_hi: "#F7F2EA",
      accent: "#E8A15C", accent_alt: "#C9BE8A", accent_warm: "#F0C089",
      ok: "#A8C58A", warn: "#E8C06A", err: "#E28C84", info: "#9FBCC8",
      ansi: ["#211D19", "#E28C84", "#A8C58A", "#E8C06A", "#9FBCC8", "#C7A9C4", "#8FC4B8", "#E6DED2",
             "#6E6255", "#F0A9A2", "#C2DCA6", "#F2D68F", "#BCD4DE", "#DCC4D9", "#AEDCD1", "#F7F2EA"],
      radius: 12, radius_sm: 8, gap: 14, animation_ms: 90,
      wall_style: "gradient",
      wall_c1: "#0E0C0A", wall_c2: "#2A211A", wall_c3: "#E8A15C", wall_c4: "#C9BE8A",
      wall_intensity: 0.32, wall_grain: 0.00, wall_vignette: 0.45
    }),

    slate: inherit({
      id: "slate", name: "Slate", variant: "dark", glyph: "◈",
      description: "Cool neutral grey, with colour kept for the things that mean something.",
      bg: "#0F1215", bg_alt: "#151920", surface: "#1C2128", surface_hi: "#262D36",
      overlay: "#66737F", muted: "#76828F", subtle: "#9BA7B4", fg: "#D5DCE4", fg_hi: "#F2F5F8",
      accent: "#7FA8D4", accent_alt: "#9BB8C9", accent_warm: "#D9B78A",
      ok: "#8FC49B", warn: "#DEBE7A", err: "#DE8D8D", info: "#8FB6D4",
      ansi: ["#1C2128", "#DE8D8D", "#8FC49B", "#DEBE7A", "#7FA8D4", "#B6A3CE", "#84C3C0", "#D5DCE4",
             "#5A6672", "#EEAAAA", "#AEDCB8", "#EDD59B", "#A5C5E4", "#CFC0E0", "#A6DBD8", "#F2F5F8"],
      radius: 14, radius_sm: 8, gap: 14,
      wall_style: "mesh",
      wall_c1: "#0B0E11", wall_c2: "#1B2430", wall_c3: "#7FA8D4", wall_c4: "#9BB8C9",
      wall_intensity: 0.30, wall_grain: 0.05, wall_vignette: 0.50
    })
  };

  /* The order Settings > How it looks lists them in (src/aurshell/settings.c),
   * then the two that are on the disk but reached with `aurora set`. */
  THEMES.order = ["nocturne", "sandstone", "moss", "synthwave", "ember", "slate"];
  THEMES.inSettings = { nocturne: 1, sandstone: 1, moss: 1, synthwave: 1 };

  root.AUROS_THEMES = THEMES;
})(typeof self !== "undefined" ? self : this);
