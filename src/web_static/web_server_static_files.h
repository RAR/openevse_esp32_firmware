#include "web_server.apple_touch_icon_png.h"
#include "web_server.assets_charts_CnsZ1jie_css_gz.h"
#include "web_server.assets_charts_D9C7sDdp_js_gz.h"
#include "web_server.assets_en_CQp_yvk__js_gz.h"
#include "web_server.assets_es__KGmkpUY_js_gz.h"
#include "web_server.assets_fr_Dh1XKq7s_js_gz.h"
#include "web_server.assets_hu_DknPgdQ9_js_gz.h"
#include "web_server.assets_index_7ZbzILuw_css_gz.h"
#include "web_server.assets_index_BIJ847KH_js_gz.h"
#include "web_server.assets_rolldown_runtime_CbXtAM7H_js_gz.h"
#include "web_server.assets_vendor_xVRjmdiJ_js_gz.h"
#include "web_server.favicon_ico.h"
#include "web_server.index_html_gz.h"
#include "web_server.manifest_webmanifest.h"
#include "web_server.pwa_192x192_png.h"
#include "web_server.pwa_512x512_png.h"
#include "web_server.pwa_maskable_512x512_png.h"
#include "web_server.sw_js.h"
StaticFile web_server_static_files[] = {
  { "/apple-touch-icon.png", CONTENT_APPLE_TOUCH_ICON_PNG, sizeof(CONTENT_APPLE_TOUCH_ICON_PNG) - 1, _CONTENT_TYPE_PNG, CONTENT_APPLE_TOUCH_ICON_PNG_ETAG, NULL },
  { "/assets/charts-CnsZ1jie.css", CONTENT_CHARTS_CNSZ1JIE_CSS_GZ, sizeof(CONTENT_CHARTS_CNSZ1JIE_CSS_GZ) - 1, _CONTENT_TYPE_CSS, CONTENT_CHARTS_CNSZ1JIE_CSS_GZ_ETAG, "gzip" },
  { "/assets/charts-D9C7sDdp.js", CONTENT_CHARTS_D9C7SDDP_JS_GZ, sizeof(CONTENT_CHARTS_D9C7SDDP_JS_GZ) - 1, _CONTENT_TYPE_JS, CONTENT_CHARTS_D9C7SDDP_JS_GZ_ETAG, "gzip" },
  { "/assets/en-CQp_yvk-.js", CONTENT_EN_CQP_YVK__JS_GZ, sizeof(CONTENT_EN_CQP_YVK__JS_GZ) - 1, _CONTENT_TYPE_JS, CONTENT_EN_CQP_YVK__JS_GZ_ETAG, "gzip" },
  { "/assets/es--KGmkpUY.js", CONTENT_ES__KGMKPUY_JS_GZ, sizeof(CONTENT_ES__KGMKPUY_JS_GZ) - 1, _CONTENT_TYPE_JS, CONTENT_ES__KGMKPUY_JS_GZ_ETAG, "gzip" },
  { "/assets/fr-Dh1XKq7s.js", CONTENT_FR_DH1XKQ7S_JS_GZ, sizeof(CONTENT_FR_DH1XKQ7S_JS_GZ) - 1, _CONTENT_TYPE_JS, CONTENT_FR_DH1XKQ7S_JS_GZ_ETAG, "gzip" },
  { "/assets/hu-DknPgdQ9.js", CONTENT_HU_DKNPGDQ9_JS_GZ, sizeof(CONTENT_HU_DKNPGDQ9_JS_GZ) - 1, _CONTENT_TYPE_JS, CONTENT_HU_DKNPGDQ9_JS_GZ_ETAG, "gzip" },
  { "/assets/index-7ZbzILuw.css", CONTENT_INDEX_7ZBZILUW_CSS_GZ, sizeof(CONTENT_INDEX_7ZBZILUW_CSS_GZ) - 1, _CONTENT_TYPE_CSS, CONTENT_INDEX_7ZBZILUW_CSS_GZ_ETAG, "gzip" },
  { "/assets/index-BIJ847KH.js", CONTENT_INDEX_BIJ847KH_JS_GZ, sizeof(CONTENT_INDEX_BIJ847KH_JS_GZ) - 1, _CONTENT_TYPE_JS, CONTENT_INDEX_BIJ847KH_JS_GZ_ETAG, "gzip" },
  { "/assets/rolldown-runtime-CbXtAM7H.js", CONTENT_ROLLDOWN_RUNTIME_CBXTAM7H_JS_GZ, sizeof(CONTENT_ROLLDOWN_RUNTIME_CBXTAM7H_JS_GZ) - 1, _CONTENT_TYPE_JS, CONTENT_ROLLDOWN_RUNTIME_CBXTAM7H_JS_GZ_ETAG, "gzip" },
  { "/assets/vendor-xVRjmdiJ.js", CONTENT_VENDOR_XVRJMDIJ_JS_GZ, sizeof(CONTENT_VENDOR_XVRJMDIJ_JS_GZ) - 1, _CONTENT_TYPE_JS, CONTENT_VENDOR_XVRJMDIJ_JS_GZ_ETAG, "gzip" },
  { "/favicon.ico", CONTENT_FAVICON_ICO, sizeof(CONTENT_FAVICON_ICO) - 1, _CONTENT_TYPE_ICO, CONTENT_FAVICON_ICO_ETAG, NULL },
  { "/index.html", CONTENT_INDEX_HTML_GZ, sizeof(CONTENT_INDEX_HTML_GZ) - 1, _CONTENT_TYPE_HTML, CONTENT_INDEX_HTML_GZ_ETAG, "gzip" },
  { "/manifest.webmanifest", CONTENT_MANIFEST_WEBMANIFEST, sizeof(CONTENT_MANIFEST_WEBMANIFEST) - 1, _CONTENT_TYPE_MANIFEST, CONTENT_MANIFEST_WEBMANIFEST_ETAG, NULL },
  { "/pwa-192x192.png", CONTENT_PWA_192X192_PNG, sizeof(CONTENT_PWA_192X192_PNG) - 1, _CONTENT_TYPE_PNG, CONTENT_PWA_192X192_PNG_ETAG, NULL },
  { "/pwa-512x512.png", CONTENT_PWA_512X512_PNG, sizeof(CONTENT_PWA_512X512_PNG) - 1, _CONTENT_TYPE_PNG, CONTENT_PWA_512X512_PNG_ETAG, NULL },
  { "/pwa-maskable-512x512.png", CONTENT_PWA_MASKABLE_512X512_PNG, sizeof(CONTENT_PWA_MASKABLE_512X512_PNG) - 1, _CONTENT_TYPE_PNG, CONTENT_PWA_MASKABLE_512X512_PNG_ETAG, NULL },
  { "/sw.js", CONTENT_SW_JS, sizeof(CONTENT_SW_JS) - 1, _CONTENT_TYPE_JS, CONTENT_SW_JS_ETAG, NULL },
};
