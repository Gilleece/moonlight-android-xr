package com.limelight.nvstream.usb;

import android.content.Context;
import android.net.Network;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Build;

import com.limelight.LimeLog;

import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.UnknownHostException;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.Executor;

/**
 * Discovers hosts (Sunshine's _nvstream._tcp service) on the USB link.
 *
 * The USB link is a point-to-point link-local segment (fe80::/64) whose addresses are not
 * knowable in advance, so the peer has to be found over mDNS, which matches what Meta's
 * documentation describes.
 *
 * The important part: the default NsdManager.discoverServices() only looks on the system
 * default network (normally Wi-Fi) and finds nothing on the USB link. The Network-scoped
 * overload from API 34 has to be used instead, passing the USB Network explicitly:
 *
 *     discoverServices(String serviceType, int protocol, Network network,
 *                      Executor executor, DiscoveryListener listener)
 *
 * Confirmed on Quest 3 / Horizon OS 2.7 to find Sunshine on usb0, with the resolved address
 * already carrying a scope so it can be handed to NvHTTP and the native side as-is.
 *
 * On re-scanning: NsdManager only reports onServiceFound() for services that already exist
 * at the moment discoverServices() starts. If the discovery session is never reopened, a host
 * the user deleted from the PC list is never reported again, because the host has been online
 * the whole time and produces no new onServiceFound(). The caller therefore calls start()
 * periodically to force a re-scan, and this class turns that into a safe state machine.
 */
public class UsbHostDiscovery {
    private static final String SERVICE_TYPE = "_nvstream._tcp";

    public interface Listener {
        /**
         * @param name    service name, normally the host name
         * @param address peer address, carrying a %iface suffix when link-local
         * @param port    service port
         */
        void onHostFound(String name, String address, int port);

        /** Discovery failed, for example because the system lacks Network-scoped discovery. */
        void onDiscoveryError(String message);
    }

    private final NsdManager nsdManager;
    private final Executor executor;

    private NsdManager.DiscoveryListener discoveryListener;
    private boolean discovering;

    /** Target network and callback of the current discovery session, reused on re-scan. */
    private Network network;
    private Listener listener;

    /**
     * A stop has been requested and we are waiting for onDiscoveryStopped() before starting
     * a fresh session.
     *
     * stopServiceDiscovery() is asynchronous: calling discoverServices() before it has
     * finished makes the system return FAILURE_ALREADY_ACTIVE(4) and discovery never starts.
     * A re-scan therefore has to be the chain "stop, wait for the callback, start again" and
     * never stop() immediately followed by start().
     */
    private boolean restartPending;

    public UsbHostDiscovery(Context context, Executor executor) {
        this.nsdManager = (NsdManager) context.getApplicationContext()
                .getSystemService(Context.NSD_SERVICE);
        this.executor = executor;
    }

    /** Network-scoped discovery needs API 34; older versions can only fall back to the default network. */
    public boolean isNetworkScopedDiscoverySupported() {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE && nsdManager != null;
    }

    public boolean isDiscovering() {
        return discovering;
    }

    /**
     * Start discovery, or schedule a re-scan when one is already running.
     *
     * The re-scan is asynchronous (see restartPending), so the caller does not have to stop()
     * first.
     */
    public void start(final Network network, final Listener listener) {
        this.network = network;
        this.listener = listener;

        if (nsdManager == null) {
            listener.onDiscoveryError("NsdManager unavailable");
            return;
        }

        if (discovering) {
            restart();
            return;
        }

        beginDiscovery();
    }

    /** Schedule a re-scan: stop the current session and start a new one once onDiscoveryStopped() arrives. */
    private void restart() {
        if (restartPending) {
            // A restart is already in flight, do not stack another one
            return;
        }

        restartPending = true;
        LimeLog.info("USB mDNS discovery restarting");

        NsdManager.DiscoveryListener l = discoveryListener;
        if (l == null) {
            // The session is already gone, just start over
            restartPending = false;
            discovering = false;
            beginDiscovery();
            return;
        }

        try {
            nsdManager.stopServiceDiscovery(l);
        } catch (Exception e) {
            LimeLog.warning("stopServiceDiscovery failed: " + e);
            discoveryListener = null;
            discovering = false;
            restartPending = false;
            beginDiscovery();
        }
    }

    private void beginDiscovery() {
        if (network == null || listener == null) {
            return;
        }

        if (!isNetworkScopedDiscoverySupported()) {
            // Older releases have no Network-scoped overload. Deliberately do not fall back
            // to the default network: that would only repeat the discovery already running
            // on Wi-Fi and confuse the user.
            LimeLog.warning("Network-scoped NSD requires API 34+, current is "
                    + Build.VERSION.SDK_INT);
            listener.onDiscoveryError("Network-scoped NSD requires API 34+");
            return;
        }

        discoveryListener = new NsdManager.DiscoveryListener() {
            @Override
            public void onDiscoveryStarted(String serviceType) {
                LimeLog.info("USB mDNS discovery started: " + serviceType);
                discovering = true;
            }

            @Override
            public void onStartDiscoveryFailed(String serviceType, int errorCode) {
                if (discoveryListener != this) {
                    return;
                }

                LimeLog.severe("USB mDNS discovery start failed: " + errorCode);
                discoveryListener = null;
                discovering = false;
                // If it will not start, do not keep retrying in a loop; report it upwards
                restartPending = false;

                if (listener != null) {
                    listener.onDiscoveryError("discoverServices failed: " + errorCode);
                }
            }

            @Override
            public void onStopDiscoveryFailed(String serviceType, int errorCode) {
                LimeLog.warning("USB mDNS discovery stop failed: " + errorCode);

                if (discoveryListener != this) {
                    return;
                }

                discoveryListener = null;
                discovering = false;
                resumeAfterStop();
            }

            @Override
            public void onDiscoveryStopped(String serviceType) {
                LimeLog.info("USB mDNS discovery stopped: " + serviceType);

                if (discoveryListener != this) {
                    return;
                }

                discoveryListener = null;
                discovering = false;
                resumeAfterStop();
            }

            @Override
            public void onServiceLost(NsdServiceInfo serviceInfo) {
                LimeLog.info("USB mDNS service lost: " + serviceInfo.getServiceName());
            }

            @Override
            public void onServiceFound(NsdServiceInfo serviceInfo) {
                LimeLog.info("USB mDNS service found: " + serviceInfo.getServiceName()
                        + " type=" + serviceInfo.getServiceType());

                try {
                    nsdManager.resolveService(serviceInfo, new NsdManager.ResolveListener() {
                        @Override
                        public void onResolveFailed(NsdServiceInfo info, int errorCode) {
                            LimeLog.warning("USB mDNS resolve failed: " + errorCode
                                    + " for " + info.getServiceName());
                        }

                        @Override
                        public void onServiceResolved(NsdServiceInfo resolved) {
                            // All addresses have to be considered, getHost() is not enough.
                            // The host's NCM adapter may advertise IPv4 (169.254.x.x) and an
                            // IPv6 link-local at the same time, while usb0 on the Quest
                            // normally only has IPv6. getHost() picks the unreachable IPv4,
                            // which shows up as "the host is discovered but never connects".
                            InetAddress host = pickBestAddress(resolved);
                            if (host == null) {
                                LimeLog.warning("USB mDNS resolved without a usable host: "
                                        + resolved.getServiceName());
                                return;
                            }

                            // Link-local addresses need a scope. Rewrite it in numeric form
                            // (fe80::x%18) because that does not depend on
                            // NetworkInterface-by-name lookups being available, and both
                            // Java and native getaddrinfo accept it directly.
                            String address = host.getHostAddress();
                            if (host instanceof Inet6Address) {
                                Inet6Address v6 = (Inet6Address) host;
                                int scopeId = v6.getScopeId();
                                if (v6.isLinkLocalAddress() && scopeId != 0) {
                                    try {
                                        address = Inet6Address.getByAddress(
                                                null, v6.getAddress(), scopeId).getHostAddress();
                                    } catch (UnknownHostException e) {
                                        // Keep whatever form the platform gave us
                                        LimeLog.warning("USB mDNS: cannot apply numeric scope "
                                                + scopeId + ", keeping " + address);
                                    }
                                }
                                LimeLog.info("USB mDNS resolved: " + resolved.getServiceName()
                                        + " -> " + address + " (scopeId=" + scopeId + ")");
                            } else {
                                LimeLog.info("USB mDNS resolved: " + resolved.getServiceName()
                                        + " -> " + address);
                            }

                            Listener l = UsbHostDiscovery.this.listener;
                            if (l != null) {
                                l.onHostFound(resolved.getServiceName(),
                                        address, resolved.getPort());
                            }
                        }
                    });
                } catch (Exception e) {
                    LimeLog.warning("USB mDNS resolve threw: " + e);
                }
            }
        };

        try {
            nsdManager.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD,
                    network, executor, discoveryListener);
            // onDiscoveryStarted() is the real confirmation; mark it in progress now so a
            // concurrent call cannot start a second session.
            discovering = true;
        } catch (Exception e) {
            LimeLog.severe("USB mDNS discovery failed to start: " + e);
            discoveryListener = null;
            discovering = false;
            if (listener != null) {
                listener.onDiscoveryError(String.valueOf(e));
            }
        }
    }

    /** Once a stop has played out, start the pending re-scan if there is one. */
    private void resumeAfterStop() {
        if (restartPending) {
            restartPending = false;
            beginDiscovery();
        }
    }

    /**
     * Pick a usable address out of everything the service advertises.
     *
     * usb0 on the Quest normally only has an IPv6 link-local, while the host's NCM adapter may
     * advertise IPv4 (169.254.x.x) and an IPv6 link-local at the same time. getHost() returns
     * just one address and settles on the unreachable IPv4, so prefer
     * IPv6 link-local, then any other IPv6, then IPv4.
     */
    private static InetAddress pickBestAddress(NsdServiceInfo info) {
        List<InetAddress> addrs;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            addrs = info.getHostAddresses();
        } else {
            InetAddress single = info.getHost();
            addrs = single != null
                    ? Collections.singletonList(single)
                    : Collections.<InetAddress>emptyList();
        }

        if (addrs == null) {
            return null;
        }

        InetAddress v6LinkLocal = null;
        InetAddress v6Other = null;
        InetAddress v4 = null;

        for (InetAddress a : addrs) {
            if (a == null || a.isLoopbackAddress()) {
                continue;
            }

            if (a instanceof Inet6Address) {
                Inet6Address v6 = (Inet6Address) a;
                if (v6.isLinkLocalAddress()) {
                    if (v6LinkLocal == null) {
                        v6LinkLocal = v6;
                    }
                } else if (v6Other == null) {
                    v6Other = v6;
                }
            } else if (v4 == null) {
                v4 = a;
            }
        }

        if (v6LinkLocal != null) {
            return v6LinkLocal;
        }
        if (v6Other != null) {
            return v6Other;
        }
        return v4;
    }

    /** Stop discovery for good and clear all state. */
    public void stop() {
        restartPending = false;

        NsdManager.DiscoveryListener l = discoveryListener;
        // Clear the reference first: an arriving callback will recognise that it is no longer
        // the current session and return immediately.
        discoveryListener = null;
        discovering = false;

        if (l != null) {
            try {
                nsdManager.stopServiceDiscovery(l);
            } catch (Exception e) {
                LimeLog.warning("stopServiceDiscovery failed: " + e);
            }
        }

        network = null;
        listener = null;
    }
}
