#include "web_server.apple_touch_icon_png.h"
#include "web_server.assets_charts_CnsZ1jie_css_gz.h"
#include "web_server.assets_charts_D9C7sDdp_js_gz.h"
#include "web_server.assets_en_CcO4T1Pk_js_gz.h"
#include "web_server.assets_es_e4qwl5Gt_js_gz.h"
#include "web_server.assets_fr_Be5hn2FK_js_gz.h"
#include "web_server.assets_hu_6Ybvk1vz_js_gz.h"
#include "web_server.assets_index_CiFvCNic_css_gz.h"
#include "web_server.assets_index_CnklzI3g_js_gz.h"
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
  { "/apple-touch-icon.png", CONTENT_APPLE_TOUCH_ICON_PNG, sizeof(CONTENT_APPLE_TOUCH_ICON_PNG) - 1, _CONTENT_TYPE_PNG, CONTENT_APPLE_TOUCH_ICON_PNG_ETAG, false },
  { "/assets/charts-CnsZ1jie.css", CONTENT_CHARTS_CNSZ1JIE_CSS_GZ, sizeof(CONTENT_CHARTS_CNSZ1JIE_CSS_GZ) - 1, _CONTENT_TYPE_CSS, CONTENT_CHARTS_CNSZ1JIE_CSS_GZ_ETAG, true },
  { "/assets/charts-D9C7sDdp.js", CONTENT_CHARTS_D9C7SDDP_JS_GZ, sizeof(CONTENT_CHARTS_D9C7SDDP_JS_GZ) - 1, _CONTENT_TYPE_JS, CONTENT_CHARTS_D9C7SDDP_JS_GZ_ETAG, true },
  { "/assets/en-CcO4T1Pk.js", CONTENT_EN_CCO4T1PK_JS_GZ, sizeof(CONTENT_EN_CCO4T1PK_JS_GZ) - 1, _CONTENT_TYPE_JS, CONTENT_EN_CCO4T1PK_JS_GZ_ETAG, true },
  { "/assets/es-e4qwl5Gt.js", CONTENT_ES_E4QWL5GT_JS_GZ, sizeof(CONTENT_ES_E4QWL5GT_JS_GZ) - 1, _CONTENT_TYPE_JS, CONTENT_ES_E4QWL5GT_JS_GZ_ETAG, true },
  { "/assets/fr-Be5hn2FK.js", CONTENT_FR_BE5HN2FK_JS_GZ, sizeof(CONTENT_FR_BE5HN2FK_JS_GZ) - 1, _CONTENT_TYPE_JS, CONTENT_FR_BE5HN2FK_JS_GZ_ETAG, true },
  { "/assets/hu-6Ybvk1vz.js", CONTENT_HU_6YBVK1VZ_JS_GZ, sizeof(CONTENT_HU_6YBVK1VZ_JS_GZ) - 1, _CONTENT_TYPE_JS, CONTENT_HU_6YBVK1VZ_JS_GZ_ETAG, true },
  { "/assets/index-CiFvCNic.css", CONTENT_INDEX_CIFVCNIC_CSS_GZ, sizeof(CONTENT_INDEX_CIFVCNIC_CSS_GZ) - 1, _CONTENT_TYPE_CSS, CONTENT_INDEX_CIFVCNIC_CSS_GZ_ETAG, true },
  { "/assets/index-CnklzI3g.js", CONTENT_INDEX_CNKLZI3G_JS_GZ, sizeof(CONTENT_INDEX_CNKLZI3G_JS_GZ) - 1, _CONTENT_TYPE_JS, CONTENT_INDEX_CNKLZI3G_JS_GZ_ETAG, true },
  { "/assets/rolldown-runtime-CbXtAM7H.js", CONTENT_ROLLDOWN_RUNTIME_CBXTAM7H_JS_GZ, sizeof(CONTENT_ROLLDOWN_RUNTIME_CBXTAM7H_JS_GZ) - 1, _CONTENT_TYPE_JS, CONTENT_ROLLDOWN_RUNTIME_CBXTAM7H_JS_GZ_ETAG, true },
  { "/assets/vendor-xVRjmdiJ.js", CONTENT_VENDOR_XVRJMDIJ_JS_GZ, sizeof(CONTENT_VENDOR_XVRJMDIJ_JS_GZ) - 1, _CONTENT_TYPE_JS, CONTENT_VENDOR_XVRJMDIJ_JS_GZ_ETAG, true },
  { "/favicon.ico", CONTENT_FAVICON_ICO, sizeof(CONTENT_FAVICON_ICO) - 1, _CONTENT_TYPE_ICO, CONTENT_FAVICON_ICO_ETAG, false },
  { "/index.html", CONTENT_INDEX_HTML_GZ, sizeof(CONTENT_INDEX_HTML_GZ) - 1, _CONTENT_TYPE_HTML, CONTENT_INDEX_HTML_GZ_ETAG, true },
  { "/manifest.webmanifest", CONTENT_MANIFEST_WEBMANIFEST, sizeof(CONTENT_MANIFEST_WEBMANIFEST) - 1, _CONTENT_TYPE_MANIFEST, CONTENT_MANIFEST_WEBMANIFEST_ETAG, false },
  { "/pwa-192x192.png", CONTENT_PWA_192X192_PNG, sizeof(CONTENT_PWA_192X192_PNG) - 1, _CONTENT_TYPE_PNG, CONTENT_PWA_192X192_PNG_ETAG, false },
  { "/pwa-512x512.png", CONTENT_PWA_512X512_PNG, sizeof(CONTENT_PWA_512X512_PNG) - 1, _CONTENT_TYPE_PNG, CONTENT_PWA_512X512_PNG_ETAG, false },
  { "/pwa-maskable-512x512.png", CONTENT_PWA_MASKABLE_512X512_PNG, sizeof(CONTENT_PWA_MASKABLE_512X512_PNG) - 1, _CONTENT_TYPE_PNG, CONTENT_PWA_MASKABLE_512X512_PNG_ETAG, false },
  { "/sw.js", CONTENT_SW_JS, sizeof(CONTENT_SW_JS) - 1, _CONTENT_TYPE_JS, CONTENT_SW_JS_ETAG, false },
};
