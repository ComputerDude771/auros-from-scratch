#@!out=/etc/auros/colors.json mode=0644
{
  "name": "@theme_name@",
  "author": "@theme_author@",
  "variant": "@theme_variant@",
  "description": "@theme_description@",
  "palette": {
    "bg": "@bg@", "bgAlt": "@bg_alt@", "surface": "@surface@",
    "surfaceHi": "@surface_hi@", "overlay": "@overlay@",
    "muted": "@muted@", "subtle": "@subtle@", "fg": "@fg@", "fgHi": "@fg_hi@",
    "accent": "@accent@", "accentAlt": "@accent_alt@", "accentWarm": "@accent_warm@",
    "ok": "@ok@", "warn": "@warn@", "err": "@err@", "info": "@info@"
  },
  "ansi": ["@ansi0@","@ansi1@","@ansi2@","@ansi3@","@ansi4@","@ansi5@","@ansi6@","@ansi7@",
           "@ansi8@","@ansi9@","@ansi10@","@ansi11@","@ansi12@","@ansi13@","@ansi14@","@ansi15@"],
  "geometry": { "radius": @radius|int@, "radiusSm": @radius_sm|int@, "gap": @gap|int@,
                "margin": @margin|int@, "border": @border|int@,
                "barHeight": @bar_height|int@, "padding": @padding|int@ },
  "typography": { "sans": "@font_sans@", "mono": "@font_mono@",
                  "size": @font_size|int@, "lineHeight": @line_height@ },
  "effects": { "blur": @blur|int@, "blurRadius": @blur_radius|int@,
               "shadowOpacity": @shadow_opacity@, "panelOpacity": @opacity_panel@,
               "animationMs": @animation_ms|int@ }
}
