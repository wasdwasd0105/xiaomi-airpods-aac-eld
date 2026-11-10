// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) the developer
package io.aaceld.label;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

/**
 * Makes the codec badge in HyperOS Bluetooth settings read "AAC-ELD" for a
 * headset the AAC-ELD for A2DP module is streaming to.
 *
 * The badge text comes from a table compiled into the Settings app,
 * BluetoothDevicePreference.getCodecName(int). The stack module borrows the
 * Opus codec slot, type 6, which is not in that table, so Settings gets
 * "UNKNOWN" and hides the badge. This hooks the lookup: when it would return
 * "UNKNOWN", and the device's active A2DP codec is type 6 carrying the mark
 * the stack module writes into codecSpecific1, it returns "AAC-ELD" instead.
 * A genuine Opus session has no mark and is left alone, and the codec type
 * Settings passes on to its own dialogs is never changed.
 *
 * The same headset's own page gets an AAC-ELD switch and a bit rate
 * drop-down; that part is in DevicePage.
 */
public class Hook implements IXposedHookLoadPackage {
    private static final String SETTINGS = "com.android.settings";
    private static final String PREFERENCE = "com.android.settings.bluetooth.BluetoothDevicePreference";
    static final int SLOT_CODEC_TYPE = 6;                    // SOURCE_CODEC_TYPE_OPUS
    private static final long ELD_MARK = 0x004C8001L;         // Apple vendor 0x004C, codec 0x8001
    private static final int STATE_CONNECTED = 2;             // BluetoothProfile.STATE_CONNECTED
    private static final String LABEL = "AAC-ELD";

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam lpparam) {
        if (!SETTINGS.equals(lpparam.packageName)) return;
        try {
            XposedHelpers.findAndHookMethod(PREFERENCE, lpparam.classLoader, "getCodecName", int.class,
                    new XC_MethodHook() {
                        @Override
                        protected void afterHookedMethod(MethodHookParam param) {
                            try {
                                if ("UNKNOWN".equals(param.getResult()) && isAacEld(param.thisObject)) {
                                    param.setResult(LABEL);
                                }
                            } catch (Throwable t) {
                                XposedBridge.log("aaceld-label: " + t);
                            }
                        }
                    });
            XposedBridge.log("aaceld-label: hooked " + PREFERENCE + ".getCodecName");
        } catch (Throwable t) {
            XposedBridge.log("aaceld-label: could not hook, " + t);
        }
        try {
            DevicePage.install(lpparam.classLoader);
        } catch (Throwable t) {
            XposedBridge.log("aaceld-label: could not hook the device page, " + t);
        }
    }

    /** True when this row's headset is connected over A2DP on the marked codec slot. */
    private static boolean isAacEld(Object preference) {
        Object a2dp = XposedHelpers.getObjectField(preference, "mBluetoothA2dp");
        Object cached = XposedHelpers.getObjectField(preference, "mCachedDevice");
        if (a2dp == null || cached == null) return false;
        Object device = XposedHelpers.callMethod(cached, "getDevice");
        if (device == null) return false;
        Object state = XposedHelpers.callMethod(a2dp, "getConnectionState", device);
        if (!(state instanceof Integer) || (Integer) state != STATE_CONNECTED) return false;
        Object status = XposedHelpers.callMethod(a2dp, "getCodecStatus", device);
        if (status == null) return false;
        return isEldConfig(XposedHelpers.callMethod(status, "getCodecConfig"));
    }

    /** True for a BluetoothCodecConfig in the borrowed slot that carries the stack module's mark. */
    static boolean isEldConfig(Object config) {
        if (config == null) return false;
        Object type = XposedHelpers.callMethod(config, "getCodecType");
        Object specific = XposedHelpers.callMethod(config, "getCodecSpecific1");
        return type instanceof Integer && (Integer) type == SLOT_CODEC_TYPE
                && specific instanceof Long && (Long) specific == ELD_MARK;
    }
}
