package com.limelight.nvstream.usb;

import android.content.Context;
import android.content.pm.PackageManager;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;

import com.limelight.LimeLog;

/**
 * Manages the USB networking (NCM) link introduced in Meta Horizon OS 2.5.
 *
 * That link turns the headset's USB-C port into a real network interface (usb0), which can
 * stand in for ADB reverse port forwarding on a wired stream. Key properties, taken from
 * Meta's "AOSP features on Horizon OS" documentation and confirmed on-device:
 *   - Request it with the standard ConnectivityManager.requestNetwork() using
 *     TRANSPORT_USB, and remove NET_CAPABILITY_INTERNET together with
 *     NET_CAPABILITY_TRUSTED, otherwise the system never returns the link as a match.
 *   - The system may show a consent dialog, and only while the app is in the foreground.
 *     A background request is dropped after 30 seconds and the callback receives
 *     onUnavailable(), at which point the callback is already unregistered and the request
 *     released.
 *   - The link is IPv6 link-local (some Horizon OS 2.7 setups also carry IPv4) and has no
 *     internet route.
 *   - Holding the request preempts USB networking for other apps, so it has to be released
 *     once it is no longer needed.
 *
 * Confirmed on Quest 3 / Horizon OS 2.7:
 *   - Interface name usb0, reported bandwidth 5,120,000 Kbps (USB 3), no consent dialog.
 *   - We are assigned a fe80::xxxx/64. The peer address has to come from mDNS discovery,
 *     and the resolved address already carries a scope.
 */
public class UsbLinkManager {
    public interface Listener {
        /** The USB link is up; iface is normally "usb0". */
        void onUsbLinkUp(Network network, String iface);

        /** The USB link went away, either unplugged or reclaimed by the system. */
        void onUsbLinkDown();

        /** The request failed: unsupported device, denied by the user, or background timeout. */
        void onUsbLinkUnavailable();
    }

    private final Context appContext;
    private final ConnectivityManager connMgr;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private ConnectivityManager.NetworkCallback callback;
    private Network network;
    private String interfaceName;
    private Listener listener;
    private boolean requested;

    public UsbLinkManager(Context context) {
        this.appContext = context.getApplicationContext();
        this.connMgr = (ConnectivityManager) appContext.getSystemService(Context.CONNECTIVITY_SERVICE);
    }

    /**
     * Whether this device could support USB networking.
     *
     * TRANSPORT_USB is an API 31 constant, but the link itself only exists on Horizon OS 2.5
     * and later, so this can only be a rough guess. The requestNetwork() callbacks are what
     * actually decides.
     */
    public boolean isSupported() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            return false;
        }

        return appContext.checkSelfPermission(android.Manifest.permission.CHANGE_NETWORK_STATE)
                == PackageManager.PERMISSION_GRANTED;
    }

    public Network getNetwork() {
        return network;
    }

    public String getInterfaceName() {
        return interfaceName;
    }

    public boolean isUp() {
        return network != null;
    }

    /**
     * Handle of the current USB network, ready to hand to MoonBridge.setProcessNetwork().
     * Returns 0 while the link is not up yet.
     */
    public long getNetworkHandle() {
        return network != null ? network.getNetworkHandle() : 0;
    }

    /** Request the USB link. Must be called while the app is in the foreground or the request is dropped. */
    public void start(Listener listener) {
        this.listener = listener;

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            LimeLog.warning("USB networking requires API 31+, current is " + Build.VERSION.SDK_INT);
            if (listener != null) {
                listener.onUsbLinkUnavailable();
            }
            return;
        }

        if (!isSupported()) {
            LimeLog.warning("USB networking unavailable: CHANGE_NETWORK_STATE not granted");
            if (listener != null) {
                listener.onUsbLinkUnavailable();
            }
            return;
        }

        if (requested) {
            LimeLog.info("USB networking already requested");
            return;
        }

        NetworkRequest request = new NetworkRequest.Builder()
                // This link carries no internet route, so both capabilities have to go or
                // the system will not return it as a match.
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_TRUSTED)
                .addTransportType(NetworkCapabilities.TRANSPORT_USB)
                .build();

        callback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network net) {
                LimeLog.info("USB network available: " + net);
            }

            @Override
            public void onLinkPropertiesChanged(Network net, LinkProperties lp) {
                String iface = lp.getInterfaceName();
                LimeLog.info("USB link properties: iface=" + iface + " addrs=" + lp.getLinkAddresses());

                // The interface name only shows up in this callback, so the link counts as
                // ready here rather than in onAvailable().
                network = net;
                interfaceName = iface;

                if (UsbLinkManager.this.listener != null) {
                    final Listener l = UsbLinkManager.this.listener;
                    mainHandler.post(() -> l.onUsbLinkUp(net, iface));
                }
            }

            @Override
            public void onCapabilitiesChanged(Network net, NetworkCapabilities caps) {
                LimeLog.info("USB link bandwidth: down=" + caps.getLinkDownstreamBandwidthKbps()
                        + " Kbps up=" + caps.getLinkUpstreamBandwidthKbps() + " Kbps");
            }

            @Override
            public void onLost(Network net) {
                LimeLog.info("USB network lost: " + net);
                if (net.equals(network)) {
                    network = null;
                    interfaceName = null;

                    // Once the system takes the link away (cable pulled, device asleep,
                    // request preempted) this callback never fires again. Unregister and
                    // clear requested here, otherwise the next start() keeps hitting the
                    // "already requested" branch and returns, so the link can never be
                    // rebuilt and the host stays offline in the PC list.
                    releaseCallback();

                    if (UsbLinkManager.this.listener != null) {
                        mainHandler.post(UsbLinkManager.this.listener::onUsbLinkDown);
                    }
                }
            }

            @Override
            public void onUnavailable() {
                // The documentation is explicit that the callback is already unregistered
                // and the request released at this point. A retry should call
                // requestNetwork() again rather than unregisterNetworkCallback().
                LimeLog.warning("USB network unavailable (denied or background timeout)");
                requested = false;
                callback = null;
                if (UsbLinkManager.this.listener != null) {
                    mainHandler.post(UsbLinkManager.this.listener::onUsbLinkUnavailable);
                }
            }
        };

        try {
            connMgr.requestNetwork(request, callback);
            requested = true;
            LimeLog.info("Requested USB networking (TRANSPORT_USB)");
        } catch (Exception e) {
            LimeLog.severe("requestNetwork(TRANSPORT_USB) failed: " + e);
            callback = null;
            requested = false;
            if (listener != null) {
                listener.onUsbLinkUnavailable();
            }
        }
    }

    /**
     * Unregister the current callback and clear the request flag.
     *
     * The caller decides whether to also clear network / interfaceName. Clearing requested is
     * the important part: only when it is false will start() actually issue a new
     * requestNetwork().
     */
    private void releaseCallback() {
        if (callback != null) {
            try {
                connMgr.unregisterNetworkCallback(callback);
            } catch (Exception e) {
                LimeLog.warning("unregisterNetworkCallback failed: " + e);
            }
        }

        callback = null;
        requested = false;
    }

    /** Release the USB link. Holding the request preempts USB networking for other apps. */
    public void stop() {
        releaseCallback();

        network = null;
        interfaceName = null;
        listener = null;
        LimeLog.info("Released USB networking");
    }
}
