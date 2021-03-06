#!/bin/sh

$1 -MMD -MF /tmp/test${2}.d -DV8_DEPRECATION_WARNINGS -D_FILE_OFFSET_BITS=64 -DENABLE_MAC_INSTALLER -DINTERNAL_BUILD -DCHROMIUM_BUILD \
-DENABLE_UPDATE_CHANNEL -DCR_CLANG_REVISION=223108 -DTOOLKIT_VIEWS=1 -DUI_COMPOSITOR_IMAGE_TRANSPORT -DUSE_AURA=1 -DUSE_ASH=1 -DUSE_PANGO=1 \
-DUSE_CAIRO=1 -DUSE_DEFAULT_RENDER_THEME=1 -DUSE_LIBJPEG_TURBO=1 -DUSE_X11=1 -DUSE_CLIPBOARD_AURAX11=1 -DENABLE_PRE_SYNC_BACKUP \
-DENABLE_WEBRTC=1 -DENABLE_PEPPER_CDMS -DENABLE_CONFIGURATION_POLICY -DENABLE_NOTIFICATIONS -DDISTRIBUTION_CANARY -DREQUIRE_SPECIAL_INSTALL \
-DENABLE_EXTENSIONS_COMMANDLINE_FLAGS -DUSE_UDEV -DVERIFY_PAK=1 -DDONT_EMBED_BUILD_METADATA -DENABLE_TASK_MANAGER=1 -DENABLE_EXTENSIONS=1 \
-DENABLE_PLUGIN_INSTALLATION=1 -DENABLE_PLUGINS=1 -DENABLE_SESSION_SERVICE=1 -DENABLE_THEMES=1 -DENABLE_BACKGROUND=1 -DCLD_VERSION=2 \
-DENABLE_PRINTING=1 -DENABLE_BASIC_PRINTING=1 -DENABLE_PRINT_PREVIEW=1 -DENABLE_SPELLCHECK=1 -DENABLE_CAPTIVE_PORTAL_DETECTION=1 \
-DENABLE_EXPERIMENTAL_MORPHOLOGY_SEARCH=1 -DENABLE_ANTISUSANIN_SAFE_BROWSING -DENABLE_ANTISUSANIN_NETERROR -DENABLE_ANTISUSANIN_CRASH_KILL_MAC \
-DENABLE_OPERA_AUTOFILL=1 -DENABLE_SITELINKS_ANIMATION=1 -DENABLE_PROGRAM_FILES_INSTALL=1 -DENABLE_INSTALLER_FADE_OUT=1 \
-DENABLE_SORT_AND_CULL_JOURNALING -DENABLE_NATIVE_PUSH_TO_CALL -DENABLE_PUSH_TO_CALL_FOR_DESKTOP -DENABLE_PHONE_SEARCH -DENABLE_DEFERRED_MEDIA \
-DENABLE_WAKE_LOCK -DENABLE_BOOK_READER -DENABLE_DOWNLOAD_PANEL=1 -DENABLE_CONTEXT_PANEL_IN_OMNIBOX=1 -DENABLE_DEVICES_TABS_PAGE \
-DENABLE_ADDING_REFERRER_TO_SUGGEST_REQUEST -DENABLE_YADISK_INTEGRATION=1 -DENABLE_FRAME_FIND_DYNAMIC_RESTART=1 -DENABLE_SKYFIRE_SUPPORT=1 \
-DENABLE_SOCIAL_NOTIFICATIONS_CPP=1 -DENABLE_HOMELAND -DENABLE_PAGE_EXPIRATION -DENABLE_NATIVE_WORD_TRANSLATOR -DENABLE_RENDERER_PRELAUNCHER \
-DENABLE_SWITCHING_EXTENSIONS_OFF -DV8_USE_EXTERNAL_STARTUP_DATA -DV8_TARGET_ARCH_X64 -DV8_I18N_SUPPORT -DICU_UTIL_DATA_IMPL=ICU_UTIL_DATA_FILE \
-DU_USING_ICU_NAMESPACE=0 -DU_ENABLE_DYLOAD=0 -DU_STATIC_IMPLEMENTATION -DUSE_LIBPCI=1 -DUSE_GLIB=1 -DUSE_NSS=1 -DNDEBUG -DNVALGRIND \
-DDYNAMIC_ANNOTATIONS_ENABLED=0 -I. -fstack-protector --param=ssp-buffer-size=4 -Werror -pthread -fno-strict-aliasing -Wno-unused-parameter \
-Wno-missing-field-initializers -fvisibility=hidden -pipe -fPIC -Wno-reserved-user-defined-literal -Wno-unused-command-line-argument \
-fcolor-diagnostics -Wheader-hygiene -Wno-char-subscripts -Wno-unneeded-internal-declaration -Wno-covered-switch-default -Wstring-conversion \
-Wno-c++11-narrowing -Wno-deprecated-register -Wno-inconsistent-missing-override -Wno-format -Wno-unused-result -m64 -march=x86-64 -m64 \
-fno-ident -fdata-sections -ffunction-sections -funwind-tables -fdata-sections -ffunction-sections  -O3 -fno-exceptions -fno-rtti \
-fno-threadsafe-statics -fvisibility-inlines-hidden -std=gnu++11 -Wno-deprecated  -c test.cc -o /tmp/test${2}.o
