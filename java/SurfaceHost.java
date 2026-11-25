import android.graphics.PixelFormat;
import android.graphics.Region;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceControl;

import java.lang.reflect.Method;

public final class SurfaceHost {
    private static final String TAG = "SimpleImGui";
    private static final int FALLBACK_WIDTH = 1080;
    private static final int FALLBACK_HEIGHT = 2460;

    private static native void nativeStart(Surface surface, int width, int height);

    public static void main(String[] args) {
        if (Looper.myLooper() == null) {
            Looper.prepareMainLooper();
        }

        int width = parseDimension(args, 0, FALLBACK_WIDTH);
        int height = parseDimension(args, 1, FALLBACK_HEIGHT);
        String libraryPath = args != null && args.length > 2
                ? args[2]
                : "/data/local/tmp/simple_imgui_run/libsimple_imgui.so";

        SurfaceControl control = null;
        Surface surface = null;
        int status = 0;

        try {
            System.load(libraryPath);

            control = new SurfaceControl.Builder()
                    .setName("SimpleImGui")
                    .setBufferSize(width, height)
                    .setFormat(PixelFormat.TRANSLUCENT)
                    .build();

            SurfaceControl.Transaction transaction = new SurfaceControl.Transaction();
            transaction.setPosition(control, 0.0f, 0.0f);
            transaction.setLayer(control, 0x7FFFFFFF);
            transaction.setVisibility(control, true);
            applyTouchPassThrough(transaction, control);
            transaction.apply();

            surface = new Surface(control);
            nativeStart(surface, width, height);
        } catch (Throwable error) {
            Log.e(TAG, "Overlay failed", error);
            status = 1;
        } finally {
            if (surface != null) {
                surface.release();
            }
            if (control != null) {
                try {
                    SurfaceControl.Transaction transaction = new SurfaceControl.Transaction();
                    transaction.reparent(control, null);
                    transaction.apply();
                } catch (Throwable error) {
                    Log.e(TAG, "Could not detach overlay", error);
                }
                control.release();
            }
        }

        System.exit(status);
    }

    private static void applyTouchPassThrough(SurfaceControl.Transaction transaction,
                                              SurfaceControl control) {
        try {
            Method setTouchableRegion = SurfaceControl.Transaction.class.getDeclaredMethod(
                    "setTouchableRegion", SurfaceControl.class, Region.class);
            setTouchableRegion.setAccessible(true);
            setTouchableRegion.invoke(transaction, control, new Region());
        } catch (Throwable ignored) {
        }

        try {
            Method setTrustedOverlay = SurfaceControl.Transaction.class.getDeclaredMethod(
                    "setTrustedOverlay", SurfaceControl.class, boolean.class);
            setTrustedOverlay.setAccessible(true);
            setTrustedOverlay.invoke(transaction, control, true);
        } catch (Throwable ignored) {
        }
    }

    private static int parseDimension(String[] args, int index, int fallback) {
        if (args == null || args.length <= index) {
            return fallback;
        }
        try {
            int value = Integer.parseInt(args[index]);
            return value > 0 ? value : fallback;
        } catch (NumberFormatException ignored) {
            return fallback;
        }
    }
}
