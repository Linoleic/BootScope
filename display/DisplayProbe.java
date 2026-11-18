package bootscope;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.hardware.HardwareBuffer;
import android.os.SystemClock;
import android.view.SurfaceControl;

import java.lang.reflect.Method;
import java.util.Locale;

public final class DisplayProbe {
    private static final String NAME = "BootScopeDisplay";
    private static final int WIDTH = 680;
    private static final int HEIGHT = 252;
    private static final int LAYER = 0x40000001;

    public static void main(String[] args) {
        try {
            run(args);
        } catch (Exception | LinkageError error) {
            error.printStackTrace(System.err);
            System.exit(1);
        }
    }

    private static void run(String[] args) throws Exception {
        boolean preview = args.length == 1 && "--preview".equals(args[0]);
        Method get = Class.forName("android.os.SystemProperties").getMethod("get", String.class);
        if (!preview && !animationActive(get)) {
            System.out.println("exit=animation_inactive");
            return;
        }

        SurfaceControl control = new SurfaceControl.Builder()
                .setName(NAME).setOpaque(true).setHidden(true).build();
        Bitmap bitmap = null;
        try {
            bitmap = Bitmap.createBitmap(WIDTH, HEIGHT, Bitmap.Config.ARGB_8888);
            Canvas canvas = new Canvas(bitmap);
            Paint paint = new Paint();
            long deadline = SystemClock.elapsedRealtime() + (preview ? 4000 : 12000);
            boolean first = true;
            while (SystemClock.elapsedRealtime() < deadline) {
                if (!preview && !animationActive(get)) {
                    System.out.println("exit=animation_ended");
                    return;
                }
                draw(canvas, paint, get, preview);
                submit(control, bitmap, first);
                if (first) {
                    System.out.println("first_frame_submitted_ms=" + SystemClock.elapsedRealtime());
                    first = false;
                }
                Thread.sleep(250);
            }
            System.out.println("exit=time_limit");
        } finally {
            try (SurfaceControl.Transaction transaction = new SurfaceControl.Transaction()) {
                transaction.setVisibility(control, false).setBuffer(control, null).apply();
            } finally {
                control.release();
                if (bitmap != null) bitmap.recycle();
                System.out.println("released_ms=" + SystemClock.elapsedRealtime());
            }
        }
    }

    private static void submit(SurfaceControl control, Bitmap bitmap, boolean first) {
        Bitmap hardware = bitmap.copy(Bitmap.Config.HARDWARE, false);
        if (hardware == null) throw new IllegalStateException("Could not allocate frame buffer");
        try (HardwareBuffer buffer = hardware.getHardwareBuffer();
                SurfaceControl.Transaction transaction = new SurfaceControl.Transaction()) {
            if (first) {
                transaction.setPosition(control, 48, 112).setLayer(control, LAYER)
                        .setVisibility(control, true);
            }
            transaction.setBuffer(control, buffer).apply();
        } finally {
            hardware.recycle();
        }
    }

    private static boolean animationActive(Method get) throws Exception {
        return "running".equals(property(get, "init.svc.bootanim"))
                && !"1".equals(property(get, "service.bootanim.exit"))
                && !"1".equals(property(get, "sys.boot_completed"));
    }

    private static String property(Method get, String name) throws Exception {
        return (String) get.invoke(null, name);
    }

    private static String state(Method get, String name) throws Exception {
        String value = property(get, "init.svc." + name);
        return (value == null || value.isEmpty()) ? "unknown" : value;
    }

    private static void draw(Canvas canvas, Paint paint, Method get, boolean preview) throws Exception {
        String uptime = String.format(Locale.ROOT, "%.2f s", SystemClock.elapsedRealtime() / 1000d);
        String graphics = state(get, "surfaceflinger");
        String runtime = state(get, "zygote");
        String animation = state(get, "bootanim");
        canvas.drawColor(0xff101b24);
        paint.setColor(0xff7be0ca);
        canvas.drawRect(0, 0, 8, HEIGHT, paint);
        BootFont.draw(canvas, paint, preview ? "BootScope / preview" : "BootScope / boot", 26, 25, 4);
        paint.setColor(0xffedf4f7);
        BootFont.draw(canvas, paint, "Uptime  " + uptime, 26, 76, 3);
        BootFont.draw(canvas, paint, "Display", 26, 114, 3);
        canvas.drawRect(28, 141, 30, 191, paint);
        canvas.drawRect(28, 158, 40, 160, paint);
        canvas.drawRect(28, 189, 40, 191, paint);
        BootFont.draw(canvas, paint, "SurfaceFlinger  " + graphics, 48, 149, 3);
        BootFont.draw(canvas, paint, "bootanim        " + animation, 48, 180, 3);
        BootFont.draw(canvas, paint, "Runtime / zygote   " + runtime, 26, 217, 3);
    }
}
