package com.yahia.livewdf;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.media.AudioManager;
import android.os.Build;
import android.os.Bundle;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

public final class LiveWdfActivity extends Activity {
    private static final String USB_PERMISSION = "com.yahia.livewdf.USB_PERMISSION";
    private static final String USB_REQUEST = "com.yahia.livewdf.USB_REQUEST";
    private static final int USB_AUDIO_STREAMING = 2;

    private UsbManager usbManager;
    private UsbDevice pendingDevice;
    private UsbDevice activeDevice;
    private UsbDeviceConnection connection;
    private int requestId;

    private native String nativeStartUsb(int fd, byte[] descriptors);
    private native void nativeStop();
    private native void nativeUsbStatus(String message);

    private final BroadcastReceiver permissionReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (!USB_PERMISSION.equals(intent.getAction()) || pendingDevice == null
                    || requestId != intent.getIntExtra(USB_REQUEST, -1))
                return;

            UsbDevice device = pendingDevice;
            pendingDevice = null;
            if (usbManager.hasPermission(device))
                openUsb(device);
            else
                nativeUsbStatus("USB permission was denied or the interface was disconnected");
        }
    };

    private final BroadcastReceiver detachReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (!UsbManager.ACTION_USB_DEVICE_DETACHED.equals(intent.getAction()))
                return;

            UsbDevice device = detachedDevice(intent);
            if (device == null)
                return;
            if (device.equals(pendingDevice))
                pendingDevice = null;
            if (device.equals(activeDevice)) {
                nativeStop();
                releaseUsbConnection();
                nativeUsbStatus("USB interface disconnected");
            }
        }
    };

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        setVolumeControlStream(AudioManager.STREAM_MUSIC);
        usbManager = (UsbManager) getSystemService(Context.USB_SERVICE);
        IntentFilter permissionFilter = new IntentFilter(USB_PERMISSION);
        IntentFilter detachFilter = new IntentFilter(UsbManager.ACTION_USB_DEVICE_DETACHED);
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(permissionReceiver, permissionFilter, Context.RECEIVER_NOT_EXPORTED);
            registerReceiver(detachReceiver, detachFilter, Context.RECEIVER_EXPORTED);
        } else {
            registerReceiver(permissionReceiver, permissionFilter);
            registerReceiver(detachReceiver, detachFilter);
        }
    }

    @Override
    protected void onStop() {
        nativeStop();
        closeConnection();
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        pendingDevice = null;
        closeConnection();
        unregisterReceiver(permissionReceiver);
        unregisterReceiver(detachReceiver);
        super.onDestroy();
    }

    // Called from the JUCE message thread through JNI.
    public void selectUsbAudio() {
        if (usbManager == null) {
            nativeUsbStatus("USB host mode is unavailable");
            return;
        }
        List<UsbDevice> devices = new ArrayList<>();
        for (UsbDevice device : usbManager.getDeviceList().values()) {
            if (hasAudioStream(device))
                devices.add(device);
        }
        if (devices.isEmpty()) {
            nativeUsbStatus("Connect a class-compliant USB audio interface first");
            return;
        }

        devices.sort(Comparator.comparing(LiveWdfActivity::deviceLabel));
        CharSequence[] labels = devices.stream().map(LiveWdfActivity::deviceLabel).toArray(CharSequence[]::new);
        new AlertDialog.Builder(this)
            .setTitle("USB audio interface")
            .setItems(labels, (dialog, index) -> requestUsbPermission(devices.get(index)))
            .setNegativeButton(android.R.string.cancel, (dialog, which) -> nativeUsbStatus("Audio stopped"))
            .setOnCancelListener(dialog -> nativeUsbStatus("Audio stopped"))
            .show();
    }

    public void releaseUsbConnection() {
        pendingDevice = null;
        closeConnection();
    }

    private void requestUsbPermission(UsbDevice device) {
        if (usbManager.hasPermission(device)) {
            openUsb(device);
            return;
        }
        pendingDevice = device;
        ++requestId;
        Intent intent = new Intent(USB_PERMISSION).setPackage(getPackageName()).putExtra(USB_REQUEST, requestId);
        PendingIntent permission = PendingIntent.getBroadcast(this, requestId, intent, PendingIntent.FLAG_IMMUTABLE);
        try {
            usbManager.requestPermission(device, permission);
        } catch (SecurityException exception) {
            pendingDevice = null;
            nativeUsbStatus("Android refused access to the USB interface");
        }
    }

    private void openUsb(UsbDevice device) {
        closeConnection();
        try {
            connection = usbManager.openDevice(device);
        } catch (SecurityException exception) {
            nativeUsbStatus("Android refused access to the USB interface");
            return;
        }
        if (connection == null) {
            nativeUsbStatus("Android could not open the USB interface");
            return;
        }

        String error = nativeStartUsb(connection.getFileDescriptor(), connection.getRawDescriptors());
        if (error != null) {
            closeConnection();
            nativeUsbStatus(error);
        } else {
            activeDevice = device;
        }
    }

    private void closeConnection() {
        if (connection != null)
            connection.close();
        connection = null;
        activeDevice = null;
    }

    private static boolean hasAudioStream(UsbDevice device) {
        for (int index = 0; index < device.getInterfaceCount(); ++index) {
            UsbInterface candidate = device.getInterface(index);
            if (candidate.getInterfaceClass() == UsbConstants.USB_CLASS_AUDIO
                    && candidate.getInterfaceSubclass() == USB_AUDIO_STREAMING)
                return true;
        }
        return false;
    }

    private static String deviceLabel(UsbDevice device) {
        String name = device.getProductName();
        return name != null ? name : String.format(java.util.Locale.ROOT, "USB audio %04x:%04x",
                                                     device.getVendorId(), device.getProductId());
    }

    @SuppressWarnings("deprecation")
    private static UsbDevice detachedDevice(Intent intent) {
        if (Build.VERSION.SDK_INT >= 33)
            return intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice.class);
        return intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
    }
}
