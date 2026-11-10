// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) the developer
package io.aaceld.label;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothCodecConfig;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.RelativeLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.lang.ref.WeakReference;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Proxy;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Collections;
import java.util.Set;
import java.util.WeakHashMap;

import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;

/**
 * Adds two rows to a headset's page in HyperOS Bluetooth settings, next to
 * Xiaomi's own codec switch: an AAC-ELD switch, a "Variable bit rate" switch
 * and a slider for the maximum bit rate. They appear only for a headset that offers Apple's AAC-ELD.
 *
 * The rows are thin: each writes one system property that the stack module
 * reads. Settings may write persist.bluetooth.* and nothing under
 * persist.aaceld.*, which is why these two live there.
 *
 *   persist.bluetooth.aaceld.enabled   the switch. The stack module declines
 *                                      the codec while it is false.
 *   persist.bluetooth.aaceld.vbr       the "Variable bit rate" switch, on
 *                                      unless false. On: variable rate up to
 *                                      the maximum, and never above 256 kbps.
 *                                      Off: constant rate at the maximum.
 *   persist.bluetooth.aaceld.bitrate   the maximum bit rate in bit/s, the
 *                                      slider. Unset or 0 is 256000. The
 *                                      encoder picks a change of either up
 *                                      within a second, while playing.
 *
 * The switch also has to make the stack choose again, or it would only take
 * effect at the next connection. It does that the way Xiaomi's own switch
 * does, with a codec preference request; see requestSwitch for the detail
 * that makes it work in both directions.
 *
 * The settings are phone-wide, not per headset: the stack is not told which
 * headset it is choosing a codec for at the point where the module decides.
 */
final class DevicePage {
    private static final String TAG = "aaceld-label: ";
    private static final String PAGE = "com.android.settings.bluetooth.DeviceProfilesSettings";
    private static final String PROP_ENABLED = "persist.bluetooth.aaceld.enabled";
    private static final String PROP_BITRATE = "persist.bluetooth.aaceld.bitrate";
    private static final String PROP_VBR = "persist.bluetooth.aaceld.vbr";
    private static final String KEY_SWITCH = "aaceld_enabled";
    private static final String KEY_VBR = "aaceld_vbr";
    private static final String KEY_BITRATE = "aaceld_bitrate";
    // The slider's four stops. 256 is the most the headset advertises and the
    // most variable rate will use; 288 and 320 are constant rate only.
    private static final int[] KBPS_STOPS = {192, 256, 288, 320};
    private static final int KBPS_DEFAULT = 256;
    private static final int KBPS_VBR_CEILING = 256;
    private static final String TICKS_TAG = "aaceld_ticks";
    // Xiaomi's own seek-bar row, the one its Bluetooth volume slider extends.
    private static final String SEEKBAR_ROW = "com.android.settings.widget.SeekBarPreference";
    private static final String KEY_XIAOMI_CODEC_SWITCH = "ldac_pre";
    private static final String KEY_XIAOMI_ABS_VOLUME = "abs_volume_pre";
    private static final String XIAOMI_ABS_VOLUME_RECORD = "ABSOLUTEVOLUME";

    private static final int CODEC_SBC = 0;
    private static final int CODEC_AAC = 1;
    private static final int PRIORITY_HIGHEST = 1000 * 1000;

    private static Object sA2dp;                     // BluetoothA2dp proxy, kept for the process
    private static boolean sProxyRequested;
    private static Page sPage;
    private static final Set<ClassLoader> sPluginLoaders =
            Collections.newSetFromMap(new WeakHashMap<ClassLoader, Boolean>());

    // Xiaomi draws a headset's page in one of two places. A headset it does
    // not know gets the stock page, DeviceProfilesSettings in the Settings
    // app. AirPods it has in its database get a page of their own from the
    // headset plugin, a separately downloaded split that Settings loads into
    // its own class loader: plugin.settings.java.airpods.MiuiAirpodsFragment,
    // whose codec row is managed by AirPodsCodec. The two are built alike, so
    // the rows are the same and only how the page is reached differs.
    private static final String PLUGIN_PREFIX = "plugin.settings.java.";
    private static final String PLUGIN_CODEC = "plugin.settings.java.airpods.AirPodsCodec";
    private static final String PLUGIN_FRAGMENT = "plugin.settings.java.airpods.MiuiAirpodsFragment";
    private static final String KEY_PLUGIN_CODEC_SWITCH = "aac_pre";
    private static final String KEY_PLUGIN_SPATIAL = "SpatialAudio";

    /** What the rows need from whichever page they are on. */
    private static final class Page {
        final WeakReference<Object> fragment;   // has getActivity() and getPreferenceScreen()
        final WeakReference<Object> group;      // the preference group Xiaomi's codec switch is in
        final Object cached;                    // CachedBluetoothDevice
        final Object a2dp;                      // the page's own BluetoothA2dp, a fallback
        final String codecSwitchKey;            // Xiaomi's AAC switch on this page
        final boolean plugin;

        private Page(Object fragment, Object group, Object cached, Object a2dp, String key, boolean plugin) {
            this.fragment = new WeakReference<>(fragment);
            this.group = new WeakReference<>(group);
            this.cached = cached;
            this.a2dp = a2dp;
            this.codecSwitchKey = key;
            this.plugin = plugin;
        }

        static Page stock(Object fragment) {
            return new Page(fragment, XposedHelpers.getObjectField(fragment, "mCodecContainer"),
                    XposedHelpers.getObjectField(fragment, "mCachedDevice"),
                    XposedHelpers.getObjectField(fragment, "mBluetoothA2dp"),
                    KEY_XIAOMI_CODEC_SWITCH, false);
        }

        static Page plugin(Object codec, Object fragment, Object group) {
            Object a2dp = XposedHelpers.getObjectField(codec, "mBluetoothA2dp");
            if (a2dp instanceof WeakReference) a2dp = ((WeakReference<?>) a2dp).get();
            return new Page(fragment, group, XposedHelpers.getObjectField(codec, "mCachedDevice"),
                    a2dp, KEY_PLUGIN_CODEC_SWITCH, true);
        }

        Object activity() {
            Object f = fragment.get();
            return f == null ? null : XposedHelpers.callMethod(f, "getActivity");
        }

        Object screen() {
            Object f = fragment.get();
            return f == null ? null : XposedHelpers.callMethod(f, "getPreferenceScreen");
        }

        BluetoothDevice device() {
            return cached == null ? null : (BluetoothDevice) XposedHelpers.callMethod(cached, "getDevice");
        }
    }
    private static boolean sOwnRequest;
    private static boolean sKeepAacRecord;

    private DevicePage() {}

    static void install(ClassLoader cl) {
        try {
            XposedHelpers.findAndHookMethod(SEEKBAR_ROW, cl, "onBindViewHolder",
                    "androidx.preference.PreferenceViewHolder", new XC_MethodHook() {
                        @Override
                        protected void afterHookedMethod(MethodHookParam param) {
                            try {
                                decorateSlider(param.thisObject, param.args[0]);
                            } catch (Throwable t) {
                                XposedBridge.log(TAG + "slider: " + t);
                            }
                        }
                    });
        } catch (Throwable t) {
            XposedBridge.log(TAG + "slider hook: " + t);   // the row still works, undecorated
        }
        XposedHelpers.findAndHookMethod(PAGE, cl, "addPreferencesForSpecialCodec", new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam param) {
                try {
                    attach(Page.stock(param.thisObject));
                } catch (Throwable t) {
                    XposedBridge.log(TAG + "page: " + t);
                }
            }
        });
        XposedHelpers.findAndHookMethod("android.bluetooth.BluetoothA2dp", cl, "setCodecConfigPreference",
                BluetoothDevice.class, BluetoothCodecConfig.class, new XC_MethodHook() {
                    @Override
                    protected void beforeHookedMethod(MethodHookParam param) {
                        try {
                            if (shouldHoldBack(param.thisObject, param.args)) param.setResult(null);
                        } catch (Throwable t) {
                            XposedBridge.log(TAG + "preference filter: " + t);
                        }
                    }
                });
        // Xiaomi's page keeps a per-headset record of its own AAC switch, and
        // rewrites it from whatever codec is active each time the codec status
        // changes: AAC active means on, anything else means off. With AAC-ELD
        // active that turns the AAC switch off behind the user's back, and the
        // Bluetooth app then forces SBC the moment AAC-ELD is switched off.
        // While AAC-ELD is the active codec the record is left as the user set
        // it. Toggling the AAC switch by hand still writes it.
        XposedHelpers.findAndHookMethod(PAGE, cl, "updateCodecStatus", new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    sKeepAacRecord = eldActive(Page.stock(param.thisObject));
                } catch (Throwable t) {
                    sKeepAacRecord = false;
                }
            }

            @Override
            protected void afterHookedMethod(MethodHookParam param) {
                sKeepAacRecord = false;
            }
        });
        XposedHelpers.findAndHookMethod("com.android.settingslib.bluetooth.CachedBluetoothDevice", cl,
                "setSpecificCodecStatus", String.class, int.class, new XC_MethodHook() {
                    @Override
                    protected void beforeHookedMethod(MethodHookParam param) {
                        if (sKeepAacRecord && "AAC".equals(param.args[0])
                                && Integer.valueOf(0).equals(param.args[1])) {
                            param.setResult(null);
                        }
                    }
                });
        // "Keep volume consistent" is Xiaomi's name for AVRCP absolute volume.
        // With it off the phone sets the headset to full volume and turns the
        // music down itself, before the encoder. Xiaomi's offloaded codecs
        // take that at 24 or 32 bit. The software path AAC-ELD runs on is 16
        // bit, and FDK's encoder takes 16 bit input, so 30 to 40 dB of
        // software attenuation leaves a handful of bits: quiet passages were
        // measured at -72 to -79 dBFS going into the encoder, and the headset
        // then amplifies the damage back up. Heard as a smeared stereo image,
        // the voice drifting to one side. So while AAC-ELD is the active codec
        // the switch cannot be turned off, and it is turned on if found off.
        XposedHelpers.findAndHookMethod(PAGE, cl, "onPreferenceChange",
                "androidx.preference.Preference", Object.class, new XC_MethodHook() {
                    @Override
                    protected void beforeHookedMethod(MethodHookParam param) {
                        try {
                            Object key = XposedHelpers.callMethod(param.args[0], "getKey");
                            Page page = Page.stock(param.thisObject);
                            if (KEY_XIAOMI_ABS_VOLUME.equals(key) && Boolean.FALSE.equals(param.args[1])
                                    && eldActive(page)) {
                                explainAbsoluteVolume(page, param.args[0]);
                                param.setResult(false);
                            }
                        } catch (Throwable t) {
                            XposedBridge.log(TAG + "volume switch: " + t);
                        }
                    }
                });
        XposedBridge.log(TAG + "hooked " + PAGE);

        // The plugin's classes do not exist until Settings loads the plugin,
        // and then only in the plugin's class loader. Its activities are
        // created through Instrumentation like any other, which is where the
        // loader first shows itself.
        try {
            XposedHelpers.findAndHookMethod("android.app.Instrumentation", cl, "newActivity",
                    ClassLoader.class, String.class, Intent.class, new XC_MethodHook() {
                        @Override
                        protected void afterHookedMethod(MethodHookParam param) {
                            try {
                                Object name = param.args[1];
                                Object activity = param.getResult();
                                if (activity != null && name instanceof String
                                        && ((String) name).startsWith(PLUGIN_PREFIX)) {
                                    installPlugin(activity.getClass().getClassLoader());
                                }
                            } catch (Throwable t) {
                                XposedBridge.log(TAG + "plugin: " + t);
                            }
                        }
                    });
        } catch (Throwable t) {
            XposedBridge.log(TAG + "plugin hook: " + t);
        }
    }

    /** The same three hooks as on the stock page, on the plugin's AirPods page. */
    private static synchronized void installPlugin(ClassLoader cl) {
        if (cl == null || !sPluginLoaders.add(cl)) return;
        try {
            XposedHelpers.findAndHookMethod(PLUGIN_CODEC, cl, "addPreferencesForSpecialCodec",
                    boolean.class, "androidx.preference.Preference$OnPreferenceChangeListener",
                    "androidx.preference.PreferenceGroup", new XC_MethodHook() {
                        @Override
                        protected void afterHookedMethod(MethodHookParam param) {
                            try {
                                // The listener it is handed is the fragment itself.
                                if (param.args[1] != null && param.args[2] != null) {
                                    attach(Page.plugin(param.thisObject, param.args[1], param.args[2]));
                                }
                            } catch (Throwable t) {
                                XposedBridge.log(TAG + "plugin page: " + t);
                            }
                        }
                    });
            XposedHelpers.findAndHookMethod(PLUGIN_CODEC, cl, "updateCodecStatus", new XC_MethodHook() {
                @Override
                protected void beforeHookedMethod(MethodHookParam param) {
                    try {
                        sKeepAacRecord = eldActive(Page.plugin(param.thisObject, null, null));
                    } catch (Throwable t) {
                        sKeepAacRecord = false;
                    }
                }

                @Override
                protected void afterHookedMethod(MethodHookParam param) {
                    sKeepAacRecord = false;
                }
            });
            XposedHelpers.findAndHookMethod(PLUGIN_FRAGMENT, cl, "onPreferenceChange",
                    "androidx.preference.Preference", Object.class, new XC_MethodHook() {
                        @Override
                        protected void beforeHookedMethod(MethodHookParam param) {
                            try {
                                Object key = XposedHelpers.callMethod(param.args[0], "getKey");
                                Page page = sPage;
                                if (page == null || page.fragment.get() != param.thisObject) return;
                                if (KEY_XIAOMI_ABS_VOLUME.equals(key) && Boolean.FALSE.equals(param.args[1])
                                        && eldActive(page)) {
                                    explainAbsoluteVolume(page, param.args[0]);
                                    param.setResult(false);
                                }
                                // "Dimensional audio" is the phone's spatializer. Its output
                                // exists only in the primary audio module, which reaches a
                                // Bluetooth headset through the hardware encoder; AAC-ELD is
                                // encoded in software and plays through the separate
                                // "bluetooth" audio module, which has no spatializer output.
                                // The plugin then just lets the switch fall back to off, so
                                // say why. Its own handling still runs.
                                if (KEY_PLUGIN_SPATIAL.equals(key) && Boolean.TRUE.equals(param.args[1])
                                        && eldActive(page)) {
                                    explainSpatial(page, param.args[0]);
                                }
                            } catch (Throwable t) {
                                XposedBridge.log(TAG + "plugin volume switch: " + t);
                            }
                        }
                    });
            XposedBridge.log(TAG + "hooked the headset plugin's AirPods page");
        } catch (Throwable t) {
            XposedBridge.log(TAG + "plugin hooks: " + t);
        }
    }

    // ---- the page -------------------------------------------------------

    private static void attach(Page page) {
        sPage = page;
        if (sA2dp != null) {
            populate(page);
            return;
        }
        if (sProxyRequested) return;
        Object activity = page.activity();
        if (!(activity instanceof Context)) {
            XposedBridge.log(TAG + "page has no activity yet");
            return;
        }
        Context context = (Context) activity;
        BluetoothManager manager = context.getSystemService(BluetoothManager.class);
        BluetoothAdapter adapter = manager == null ? null : manager.getAdapter();
        if (adapter == null) return;
        sProxyRequested = adapter.getProfileProxy(context.getApplicationContext(),
                new BluetoothProfile.ServiceListener() {
                    @Override
                    public void onServiceConnected(int profile, BluetoothProfile proxy) {
                        sA2dp = proxy;
                        Page current = sPage;
                        try {
                            if (current != null) populate(current);
                        } catch (Throwable t) {
                            XposedBridge.log(TAG + "page: " + t);
                        }
                    }

                    @Override
                    public void onServiceDisconnected(int profile) {
                        sA2dp = null;
                        sProxyRequested = false;
                    }
                }, BluetoothProfile.A2DP);
    }

    private static void populate(Page page) {
        if (page.activity() == null) return;
        Object screen = page.screen();
        if (screen == null) return;
        if (XposedHelpers.callMethod(screen, "findPreference", (CharSequence) KEY_SWITCH) != null) return;

        Object group = page.group.get();
        if (group == null) group = screen;
        Context context = (Context) XposedHelpers.callMethod(group, "getContext");
        BluetoothDevice device = page.device();
        if (device == null || !offersEld(context, device)) return;

        // The preference classes have to be the ones this page's own rows are
        // made of. The plugin carries a copy of androidx.preference; which
        // copy is live is decided by its class loader, so it is read off the
        // group the rows go into.
        ClassLoader cl = preferenceLoader(group);
        boolean zh = "zh".equals(Locale.getDefault().getLanguage());
        boolean enabled = getBoolean(PROP_ENABLED, true);

        // Maximum bit rate. The row has no value field of its own, so the
        // figure goes in the title and follows the thumb; decorateSlider adds
        // the stop labels under the bar.
        final Object bitrate = XposedHelpers.newInstance(XposedHelpers.findClass(SEEKBAR_ROW, cl), context);
        XposedHelpers.callMethod(bitrate, "setKey", KEY_BITRATE);
        XposedHelpers.callMethod(bitrate, "setPersistent", false);
        XposedHelpers.callMethod(bitrate, "setMin", 0);
        XposedHelpers.callMethod(bitrate, "setMax", KBPS_STOPS.length - 1);
        XposedHelpers.callMethod(bitrate, "setProgress", currentStop());
        XposedHelpers.callMethod(bitrate, "setContinuousUpdates", true);
        XposedHelpers.callMethod(bitrate, "setTitle",
                (CharSequence) bitrateTitle(zh, KBPS_STOPS[currentStop()], getBoolean(PROP_VBR, true)));
        XposedHelpers.callMethod(bitrate, "setEnabled", enabled);
        XposedHelpers.callMethod(bitrate, "setOnPreferenceChangeListener",
                listener(cl, (preference, value) -> {
                    int kbps = KBPS_STOPS[Math.max(0, Math.min(KBPS_STOPS.length - 1, (Integer) value))];
                    set(PROP_BITRATE, String.valueOf(kbps * 1000));
                    XposedHelpers.callMethod(preference, "setTitle",
                            (CharSequence) bitrateTitle(zh, kbps, getBoolean(PROP_VBR, true)));
                    return true;
                }));

        final Object vbr = XposedHelpers.newInstance(
                XposedHelpers.findClass("androidx.preference.CheckBoxPreference", cl), context);
        XposedHelpers.callMethod(vbr, "setKey", KEY_VBR);
        XposedHelpers.callMethod(vbr, "setTitle", (CharSequence) (zh ? "可变码率" : "Variable bit rate"));
        XposedHelpers.callMethod(vbr, "setSummary", (CharSequence) (zh
                ? "按音乐需要分配码率，最高 " + KBPS_VBR_CEILING + " kbps"
                : "Uses fewer bits when the music allows, " + KBPS_VBR_CEILING + " kbps at most"));
        XposedHelpers.callMethod(vbr, "setPersistent", false);
        XposedHelpers.callMethod(vbr, "setChecked", getBoolean(PROP_VBR, true));
        XposedHelpers.callMethod(vbr, "setEnabled", enabled);
        XposedHelpers.callMethod(vbr, "setOnPreferenceChangeListener",
                listener(cl, (preference, value) -> {
                    boolean on = Boolean.TRUE.equals(value);
                    set(PROP_VBR, on ? "true" : "false");
                    XposedHelpers.callMethod(bitrate, "setTitle",
                            (CharSequence) bitrateTitle(zh, KBPS_STOPS[currentStop()], on));
                    XposedBridge.log(TAG + "variable bit rate " + (on ? "on" : "off"));
                    return true;
                }));

        Object toggle = XposedHelpers.newInstance(
                XposedHelpers.findClass("androidx.preference.CheckBoxPreference", cl), context);
        XposedHelpers.callMethod(toggle, "setKey", KEY_SWITCH);
        XposedHelpers.callMethod(toggle, "setTitle", (CharSequence) "AAC-ELD");
        XposedHelpers.callMethod(toggle, "setSummary", (CharSequence) (zh
                ? "Apple 低延迟编码，开启后代替 AAC"
                : "Apple's low-delay codec. Used instead of AAC when on."));
        XposedHelpers.callMethod(toggle, "setPersistent", false);
        XposedHelpers.callMethod(toggle, "setChecked", enabled);
        XposedHelpers.callMethod(toggle, "setOnPreferenceChangeListener",
                listener(cl, (preference, value) -> {
                    boolean on = Boolean.TRUE.equals(value);
                    set(PROP_ENABLED, on ? "true" : "false");
                    XposedHelpers.callMethod(bitrate, "setEnabled", on);
                    XposedHelpers.callMethod(vbr, "setEnabled", on);
                    requestSwitch(page, on);
                    if (on) {
                        // Once the stack has had time to move to AAC-ELD.
                        new Handler(Looper.getMainLooper()).postDelayed(() -> {
                            try {
                                if (eldActive(page)) ensureAbsoluteVolume(page);
                            } catch (Throwable t) {
                                XposedBridge.log(TAG + "volume switch: " + t);
                            }
                        }, 1500);
                    }
                    return true;
                }));

        // The plugin gives its rows explicit positions, and a row without one
        // goes to the top of the card. Ours follow its AAC switch.
        if (page.plugin) {
            Object aac = XposedHelpers.callMethod(screen, "findPreference", (CharSequence) page.codecSwitchKey);
            if (aac != null) {
                int order = (Integer) XposedHelpers.callMethod(aac, "getOrder");
                XposedHelpers.callMethod(toggle, "setOrder", order + 1);
                XposedHelpers.callMethod(vbr, "setOrder", order + 2);
                XposedHelpers.callMethod(bitrate, "setOrder", order + 3);
            }
        }
        XposedHelpers.callMethod(group, "addPreference", toggle);
        XposedHelpers.callMethod(group, "addPreference", vbr);
        XposedHelpers.callMethod(group, "addPreference", bitrate);
        if (eldActive(page)) ensureAbsoluteVolume(page);
    }

    /**
     * Turns Xiaomi's "Keep volume consistent" on for this headset if it is
     * off, the same three steps its own confirmation dialog takes.
     */
    private static void ensureAbsoluteVolume(Page page) {
        Object activity = page.activity();
        if (activity == null) return;
        Object screen = page.screen();
        Object row = screen == null ? null
                : XposedHelpers.callMethod(screen, "findPreference", (CharSequence) KEY_XIAOMI_ABS_VOLUME);
        Object cached = page.cached;
        if (row == null || cached == null) return;      // this headset has no such switch
        Object record = XposedHelpers.callMethod(cached, "getSpecificCodecStatus", XIAOMI_ABS_VOLUME_RECORD);
        if (Integer.valueOf(1).equals(record)) return;
        XposedHelpers.callMethod(row, "setChecked", true);
        XposedHelpers.callMethod(cached, "setSpecificCodecStatus", XIAOMI_ABS_VOLUME_RECORD, 1);
        if (page.plugin) {
            // What the plugin's AbsVolumeManager.sendBroadcastEnableOrDisable sends.
            BluetoothDevice device = page.device();
            if (device != null && activity instanceof Context) {
                Intent intent = new Intent("miui.bluetooth.absolute_volume_enable_disable");
                intent.setPackage("com.android.bluetooth");
                intent.putExtra("absolute_volume_mac", device.getAddress());
                intent.putExtra("absolute_volume_value", true);
                ((Context) activity).sendBroadcast(intent);
            }
        } else {
            XposedHelpers.callMethod(page.fragment.get(), "sendBroadcastEnableOrDisable", true);
        }
        XposedBridge.log(TAG + "turned absolute volume on for AAC-ELD");
        explainAbsoluteVolume(page, row);
    }

    private static void explainAbsoluteVolume(Page page, Object row) {
        Object activity = page.activity();
        if (!(activity instanceof Context)) return;
        Object title = XposedHelpers.callMethod(row, "getTitle");
        boolean zh = "zh".equals(Locale.getDefault().getLanguage());
        String text = zh
                ? "AAC-ELD 需要保持“" + title + "”开启，否则音质下降"
                : "AAC-ELD needs \"" + title + "\" on, or sound quality drops.";
        Toast.makeText((Context) activity, text, Toast.LENGTH_LONG).show();
    }

    private static void explainSpatial(Page page, Object row) {
        Object activity = page.activity();
        if (!(activity instanceof Context)) return;
        Object title = XposedHelpers.callMethod(row, "getTitle");
        boolean zh = "zh".equals(Locale.getDefault().getLanguage());
        // Two lines is all a toast gets here.
        String text = zh
                ? "AAC-ELD 开启时“" + title + "”不可用，关闭 AAC-ELD 后可用"
                : "Not available while AAC-ELD is on. Turn AAC-ELD off to use " + title + ".";
        Toast.makeText((Context) activity, text, Toast.LENGTH_LONG).show();
        XposedBridge.log(TAG + "explained why " + title + " will not turn on with AAC-ELD");
    }

    /** True when this page's headset is connected and AAC-ELD is its active codec. */
    private static boolean eldActive(Page page) {
        BluetoothDevice device = page.device();
        Object a2dp = sA2dp != null ? sA2dp : page.a2dp;
        if (device == null || a2dp == null) return false;
        Object status = XposedHelpers.callMethod(a2dp, "getCodecStatus", device);
        return status != null && Hook.isEldConfig(XposedHelpers.callMethod(status, "getCodecConfig"));
    }

    /** The class loader that defined the androidx Preference class this group is built on. */
    private static ClassLoader preferenceLoader(Object group) {
        for (Class<?> c = group.getClass(); c != null; c = c.getSuperclass()) {
            if ("androidx.preference.Preference".equals(c.getName())) return c.getClassLoader();
        }
        return group.getClass().getClassLoader();
    }

    /** The slider stop nearest the maximum bit rate setting. */
    private static int currentStop() {
        int kbps = KBPS_DEFAULT;
        try {
            int bits = Integer.parseInt(get(PROP_BITRATE, "0").trim());
            if (bits > 0) kbps = bits / 1000;
        } catch (NumberFormatException ignored) {
            // an unreadable value is treated as unset
        }
        int best = 0;
        for (int i = 1; i < KBPS_STOPS.length; i++) {
            if (Math.abs(KBPS_STOPS[i] - kbps) < Math.abs(KBPS_STOPS[best] - kbps)) best = i;
        }
        return best;
    }

    private static String bitrateTitle(boolean zh, int kbps, boolean vbr) {
        String title = (zh ? "最高码率：" : "Max bit rate: ") + kbps + " kbps";
        if (vbr && kbps > KBPS_VBR_CEILING) {
            title += zh ? "（可变码率按 " + KBPS_VBR_CEILING + "）" : " (variable uses " + KBPS_VBR_CEILING + ")";
        }
        return title;
    }

    /**
     * Xiaomi's seek-bar row is a title and a bar that runs to the bottom edge
     * of the card. For the bit-rate slider the four stops are labelled under
     * the bar, the one in use in the title's colour, and the row gets room
     * below. Rows are recycled, so anything else bound to the same views has
     * the labels taken off again.
     */
    private static void decorateSlider(Object preference, Object holder) {
        View item = (View) XposedHelpers.getObjectField(holder, "itemView");
        if (item == null) return;
        int barId = item.getResources().getIdentifier("seekbar", "id", "com.android.settings");
        View bar = barId == 0 ? null : item.findViewById(barId);
        if (bar == null || !(bar.getParent() instanceof RelativeLayout)) return;
        RelativeLayout parent = (RelativeLayout) bar.getParent();
        View old = parent.findViewWithTag(TICKS_TAG);
        boolean ours = KEY_BITRATE.equals(XposedHelpers.callMethod(preference, "getKey"));
        if (!ours) {
            if (old != null) parent.removeView(old);
            return;
        }

        Context context = item.getContext();
        float dp = context.getResources().getDisplayMetrics().density;
        TextView title = item.findViewById(android.R.id.title);
        TextView summary = item.findViewById(android.R.id.summary);
        LinearLayout ticks = old instanceof LinearLayout ? (LinearLayout) old : null;
        if (ticks == null) {
            if (old != null) parent.removeView(old);
            ticks = new LinearLayout(context);
            ticks.setTag(TICKS_TAG);
            ticks.setOrientation(LinearLayout.HORIZONTAL);
            for (int i = 0; i < KBPS_STOPS.length; i++) {
                if (i > 0) {
                    View gap = new View(context);
                    ticks.addView(gap, new LinearLayout.LayoutParams(0, 1, 1f));
                }
                TextView label = new TextView(context);
                label.setText(String.valueOf(KBPS_STOPS[i]));
                label.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
                ticks.addView(label, new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            }
            RelativeLayout.LayoutParams lp = new RelativeLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.addRule(RelativeLayout.BELOW, barId);
            lp.topMargin = (int) (6 * dp);
            parent.addView(ticks, lp);
        }
        int stop = (Integer) XposedHelpers.callMethod(preference, "getProgress");
        boolean enabled = (Boolean) XposedHelpers.callMethod(preference, "isEnabled");
        for (int i = 0, n = 0; i < ticks.getChildCount(); i++) {
            if (!(ticks.getChildAt(i) instanceof TextView)) continue;
            TextView label = (TextView) ticks.getChildAt(i);
            TextView like = (n == stop && title != null) ? title : summary;
            if (like != null) label.setTextColor(like.getTextColors());
            label.setEnabled(enabled);
            n++;
        }
        int bottom = (int) (14 * dp);
        if (item.getPaddingBottom() < bottom) {
            item.setPaddingRelative(item.getPaddingStart(), item.getPaddingTop(), item.getPaddingEnd(), bottom);
        }
    }

    // ---- does this headset have the codec -------------------------------

    /**
     * While the headset is connected the stack says so itself: the module
     * lists the marked codec slot among the selectable codecs even when it is
     * switched off. A disconnected headset has no codec status, so the answer
     * from the last connection is remembered.
     */
    private static boolean offersEld(Context context, BluetoothDevice device) {
        SharedPreferences prefs = context.getApplicationContext()
                .getSharedPreferences("aaceld_label", Context.MODE_PRIVATE);
        Set<String> known = new HashSet<>(prefs.getStringSet("devices", new HashSet<String>()));
        Object status = sA2dp == null ? null : XposedHelpers.callMethod(sA2dp, "getCodecStatus", device);
        if (status == null) return known.contains(device.getAddress());

        boolean offers = Hook.isEldConfig(XposedHelpers.callMethod(status, "getCodecConfig"));
        Object selectable = XposedHelpers.callMethod(status, "getCodecsSelectableCapabilities");
        if (!offers && selectable instanceof List) {
            for (Object config : (List<?>) selectable) {
                if (Hook.isEldConfig(config)) {
                    offers = true;
                    break;
                }
            }
        }
        if (offers != known.contains(device.getAddress())) {
            if (offers) known.add(device.getAddress()); else known.remove(device.getAddress());
            prefs.edit().putStringSet("devices", known).apply();
        }
        return offers;
    }

    // ---- making the stack choose again ----------------------------------

    /**
     * Asks the stack to move this headset on to or off AAC-ELD now.
     *
     * A codec preference names a codec and a priority, and the stack only
     * changes codec when the named one ends up with a strictly higher priority
     * than the one in use. Xiaomi's switch always asks with the "highest"
     * priority, one million, so a second request at that value loses the tie
     * against the first. Asking with one more than the highest priority the
     * headset's codecs currently hold wins every time; the stack puts the
     * codec that lost back to its default, and a reconnect starts from the
     * defaults again.
     *
     * Turning it off names AAC, or SBC when Xiaomi's own codec switch on this
     * page is off. Nothing is sent when the headset is not connected or is
     * already where it should be; the property alone covers the next
     * connection.
     */
    private static void requestSwitch(Page page, boolean on) {
        BluetoothDevice device = page.device();
        if (sA2dp == null || device == null) return;
        Object status = XposedHelpers.callMethod(sA2dp, "getCodecStatus", device);
        if (status == null) return;
        BluetoothCodecConfig current =
                (BluetoothCodecConfig) XposedHelpers.callMethod(status, "getCodecConfig");
        if (current == null || Hook.isEldConfig(current) == on) return;

        int top = current.getCodecPriority();
        boolean aacSelectable = false;
        Object selectable = XposedHelpers.callMethod(status, "getCodecsSelectableCapabilities");
        if (selectable instanceof List) {
            for (Object item : (List<?>) selectable) {
                BluetoothCodecConfig config = (BluetoothCodecConfig) item;
                top = Math.max(top, config.getCodecPriority());
                if (config.getCodecType() == CODEC_AAC) aacSelectable = true;
            }
        }

        int type = Hook.SLOT_CODEC_TYPE;
        if (!on) {
            Object screen = page.screen();
            Object xiaomi = screen == null ? null
                    : XposedHelpers.callMethod(screen, "findPreference", (CharSequence) page.codecSwitchKey);
            boolean wantsAac = xiaomi == null
                    || Boolean.TRUE.equals(XposedHelpers.callMethod(xiaomi, "isChecked"));
            type = (aacSelectable && wantsAac) ? CODEC_AAC : CODEC_SBC;
        }
        BluetoothCodecConfig request = new BluetoothCodecConfig.Builder()
                .setCodecType(type)
                .setCodecPriority(Math.max(PRIORITY_HIGHEST, top + 1))
                .build();
        XposedBridge.log(TAG + "AAC-ELD " + (on ? "on" : "off") + ", asking for codec type " + type
                + " at priority " + request.getCodecPriority());
        sOwnRequest = true;
        try {
            XposedHelpers.callMethod(sA2dp, "setCodecConfigPreference", device, request);
        } finally {
            sOwnRequest = false;
        }
    }

    /**
     * Xiaomi's codec switch on the same page asks for AAC or SBC at the
     * highest priority, which would push the headset off AAC-ELD while the
     * AAC-ELD switch still reads on. While AAC-ELD is on and in use, a request
     * from that page for AAC or SBC is held back. Xiaomi still records its own
     * switch, and it decides between AAC and SBC once AAC-ELD is turned off.
     * Requests from anywhere else, developer options included, go through.
     */
    private static boolean shouldHoldBack(Object a2dp, Object[] args) {
        if (sOwnRequest || args == null || args.length < 2) return false;
        if (!(args[0] instanceof BluetoothDevice) || !(args[1] instanceof BluetoothCodecConfig)) return false;
        int type = ((BluetoothCodecConfig) args[1]).getCodecType();
        if (type != CODEC_AAC && type != CODEC_SBC) return false;
        if (!getBoolean(PROP_ENABLED, true)) return false;
        boolean fromPage = false;
        for (StackTraceElement frame : new Throwable().getStackTrace()) {
            if (frame.getClassName().startsWith(PAGE) || frame.getClassName().startsWith(PLUGIN_PREFIX)) {
                fromPage = true;
                break;
            }
        }
        if (!fromPage) return false;
        Object status = XposedHelpers.callMethod(a2dp, "getCodecStatus", args[0]);
        if (status == null || !Hook.isEldConfig(XposedHelpers.callMethod(status, "getCodecConfig"))) return false;
        XposedBridge.log(TAG + "held back a request for codec type " + type + " while AAC-ELD is on");
        return true;
    }

    // ---- small helpers --------------------------------------------------

    private interface Change {
        boolean onChange(Object preference, Object value);
    }

    /** An androidx Preference.OnPreferenceChangeListener, which is not on the compile classpath. */
    private static Object listener(ClassLoader cl, Change change) {
        Class<?> type = XposedHelpers.findClass("androidx.preference.Preference$OnPreferenceChangeListener", cl);
        InvocationHandler handler = (proxy, method, args) -> {
            switch (method.getName()) {
                case "onPreferenceChange":
                    try {
                        return change.onChange(args[0], args[1]);
                    } catch (Throwable t) {
                        XposedBridge.log(TAG + "change: " + t);
                        return false;
                    }
                case "equals":
                    return proxy == args[0];
                case "hashCode":
                    return System.identityHashCode(proxy);
                default:
                    return "aaceld listener";
            }
        };
        return Proxy.newProxyInstance(cl, new Class<?>[] {type}, handler);
    }

    private static Class<?> systemProperties() {
        return XposedHelpers.findClass("android.os.SystemProperties", null);
    }

    private static String get(String key, String fallback) {
        return (String) XposedHelpers.callStaticMethod(systemProperties(), "get", key, fallback);
    }

    private static boolean getBoolean(String key, boolean fallback) {
        return (Boolean) XposedHelpers.callStaticMethod(systemProperties(), "getBoolean", key, fallback);
    }

    private static void set(String key, String value) {
        XposedHelpers.callStaticMethod(systemProperties(), "set", key, value);
    }
}
