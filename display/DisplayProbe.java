package bootscope;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.hardware.HardwareBuffer;
import android.os.SystemClock;
import android.view.DisplayCutout;
import android.view.SurfaceControl;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.InputStreamReader;
import java.lang.reflect.Array;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.atomic.AtomicReference;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public final class DisplayProbe {
    private static final String NAME = "BootScopeDisplay";
    private static final int WIDTH = 680;
    private static final int HEIGHT = 316;
    private static final int LAYER = 0x40000001;
    private static final int STAGE_TOTAL = 8;

    private static final String[] WATCHED_SERVICES = {
        "surfaceflinger", "bootanim", "zygote", "apexd", "artd", "wpa_supplicant"
    };
    private static final Pattern DISPLAY_ID_PATTERN = Pattern.compile("Display\\s+(\\d+)\\b");
    private static final Pattern MODE_PROP_PATTERN = Pattern.compile("(\\d+)x(\\d+)(?:@(\\d+))?");
    private static final Pattern EVENT_PATTERN = Pattern.compile(
        "^[IWEF]/([a-z0-9_]+):\\s*(\\d+)\\s*$"
    );

    private static final class StageInfo {
        final int step;
        final String label;
        final String rawMs;

        StageInfo(int step, String label, String rawMs) {
            this.step = step;
            this.label = label;
            this.rawMs = rawMs;
        }
    }

    private static final class DisplayGeometry {
        final int width;
        final int height;
        final int refreshHz;
        final int cutoutTop;
        final float posX;
        final float posY;
        final float scale;

        DisplayGeometry(int width, int height, int refreshHz, int cutoutTop) {
            this.width = width;
            this.height = height;
            this.refreshHz = refreshHz;
            this.cutoutTop = cutoutTop;
            int minDim = Math.min(width, height);
            float marginX = Math.max(24f, Math.round(width * 0.035f));
            float targetWidth = Math.min(width - marginX * 2f, Math.round(minDim * 0.62f));
            this.scale = Math.max(0.5f, targetWidth / (float) WIDTH);
            this.posX = marginX;
            float minTop = Math.round(height * 0.06f);
            float safeCutout = cutoutTop > 0 ? cutoutTop + Math.round(height * 0.012f) : 0f;
            this.posY = Math.max(minTop, safeCutout);
        }

        boolean sameTransform(DisplayGeometry other) {
            return other != null
                && Math.abs(posX - other.posX) < 0.5f
                && Math.abs(posY - other.posY) < 0.5f
                && Math.abs(scale - other.scale) < 0.005f;
        }

        String label() {
            return refreshHz > 0
                ? String.format(Locale.ROOT, "%dx%d@%d", width, height, refreshHz)
                : String.format(Locale.ROOT, "%dx%d", width, height);
        }
    }

    private static final class DisplayTracker {
        private final Method propGet;
        private final Method checkService;
        private final Method getDynamicDisplayInfo;
        private final Method getDisplayInfo;
        private final Object displayManagerGlobal;
        private long physicalDisplayId;

        DisplayTracker(Method propGet) {
            this.propGet = propGet;
            this.checkService = findMethod("android.os.ServiceManager", "checkService", String.class);
            this.getDynamicDisplayInfo = findMethod(
                SurfaceControl.class, "getDynamicDisplayInfo", long.class
            );
            Object dmg = null;
            Method gdi = null;
            try {
                Class<?> dmgClass = Class.forName("android.hardware.display.DisplayManagerGlobal");
                dmg = dmgClass.getMethod("getInstance").invoke(null);
                gdi = dmgClass.getMethod("getDisplayInfo", int.class);
            } catch (Exception ignored) {
            }
            this.displayManagerGlobal = dmg;
            this.getDisplayInfo = gdi;
            this.physicalDisplayId = queryInitialPhysicalDisplayId();
        }

        DisplayGeometry current() {
            int width = 0;
            int height = 0;
            int refreshHz = 0;
            int cutoutTop = 0;

            Object displayInfo = queryDisplayInfo();
            if (displayInfo != null) {
                width = readIntField(displayInfo, "logicalWidth");
                height = readIntField(displayInfo, "logicalHeight");
                cutoutTop = readCutoutTop(displayInfo);
                if (physicalDisplayId == 0L) {
                    physicalDisplayId = readPhysicalIdFromDisplayInfo(displayInfo);
                }
            }

            int[] sfMode = querySurfaceFlingerActiveMode();
            if (sfMode != null) {
                width = sfMode[0];
                height = sfMode[1];
                refreshHz = sfMode[2];
            }

            if (width <= 0 || height <= 0) {
                int[] propMode = parseModeProperty("vendor.display.preferred_mode");
                if (propMode == null) {
                    propMode = parseModeProperty("persist.vendor.display.primary.boot_config");
                }
                if (propMode != null) {
                    width = propMode[0];
                    height = propMode[1];
                    if (refreshHz <= 0) refreshHz = propMode[2];
                } else {
                    width = 1080;
                    height = 2340;
                }
            }
            return new DisplayGeometry(width, height, refreshHz, cutoutTop);
        }

        private Object queryDisplayInfo() {
            if (displayManagerGlobal == null || getDisplayInfo == null) return null;
            try {
                if (checkService != null && checkService.invoke(null, "display") == null) {
                    return null;
                }
                return getDisplayInfo.invoke(displayManagerGlobal, 0);
            } catch (Exception ignored) {
                return null;
            }
        }

        private int[] querySurfaceFlingerActiveMode() {
            if (physicalDisplayId == 0L || getDynamicDisplayInfo == null) return null;
            try {
                Object ddi = getDynamicDisplayInfo.invoke(null, physicalDisplayId);
                if (ddi == null) return null;
                int activeId = readIntField(ddi, "activeDisplayModeId");
                Object modes = ddi.getClass().getField("supportedDisplayModes").get(ddi);
                if (modes == null) return null;
                int len = Array.getLength(modes);
                for (int i = 0; i < len; i++) {
                    Object mode = Array.get(modes, i);
                    if (readIntField(mode, "id") == activeId) {
                        int w = readIntField(mode, "width");
                        int h = readIntField(mode, "height");
                        float fps = readFloatField(mode, "peakRefreshRate");
                        return new int[]{w, h, Math.round(fps)};
                    }
                }
            } catch (Exception ignored) {
            }
            return null;
        }

        private int[] parseModeProperty(String key) {
            try {
                String value = (String) propGet.invoke(null, key);
                if (value == null || value.isEmpty()) return null;
                Matcher m = MODE_PROP_PATTERN.matcher(value);
                if (m.find()) {
                    int w = Integer.parseInt(m.group(1));
                    int h = Integer.parseInt(m.group(2));
                    int hz = m.group(3) != null ? Integer.parseInt(m.group(3)) : 0;
                    return new int[]{w, h, hz};
                }
            } catch (Exception ignored) {
            }
            return null;
        }

        private static long queryInitialPhysicalDisplayId() {
            Process proc = null;
            try {
                proc = new ProcessBuilder("/system/bin/dumpsys", "SurfaceFlinger", "--display-id")
                    .redirectErrorStream(true).start();
                try (BufferedReader reader = new BufferedReader(
                        new InputStreamReader(proc.getInputStream(), StandardCharsets.UTF_8))) {
                    String line;
                    while ((line = reader.readLine()) != null) {
                        Matcher m = DISPLAY_ID_PATTERN.matcher(line);
                        if (m.find()) {
                            return Long.parseLong(m.group(1));
                        }
                    }
                }
            } catch (Exception ignored) {
            } finally {
                if (proc != null) proc.destroyForcibly();
            }
            return 0L;
        }

        private static long readPhysicalIdFromDisplayInfo(Object displayInfo) {
            try {
                Object address = displayInfo.getClass().getField("address").get(displayInfo);
                if (address != null) {
                    return (Long) address.getClass().getMethod("getPhysicalDisplayId").invoke(address);
                }
            } catch (Exception ignored) {
            }
            return 0L;
        }

        private static int readCutoutTop(Object displayInfo) {
            try {
                Object cutout = displayInfo.getClass().getField("displayCutout").get(displayInfo);
                if (cutout instanceof DisplayCutout) {
                    return ((DisplayCutout) cutout).getSafeInsetTop();
                }
            } catch (Exception ignored) {
            }
            return 0;
        }
    }

    public static void main(String[] args) {
        try {
            run(args);
        } catch (Exception | LinkageError error) {
            error.printStackTrace(System.err);
            System.exit(1);
        }
    }

    private static final class PsiSample {
        boolean valid;
        long totalUs;
        float avg10;
    }

    private static PsiSample readPsiFile(String path) {
        PsiSample sample = new PsiSample();
        try (BufferedReader reader = new BufferedReader(new FileReader(path))) {
            String line;
            while ((line = reader.readLine()) != null) {
                if (line.startsWith("some ")) {
                    String[] parts = line.split("\\s+");
                    float avg = 0f;
                    long total = 0L;
                    boolean hasAvg = false;
                    boolean hasTotal = false;
                    for (String p : parts) {
                        if (p.startsWith("avg10=")) {
                            avg = Float.parseFloat(p.substring(6));
                            hasAvg = true;
                        } else if (p.startsWith("total=")) {
                            total = Long.parseLong(p.substring(6));
                            hasTotal = true;
                        }
                    }
                    if (hasAvg && hasTotal) {
                        sample.valid = true;
                        sample.avg10 = avg;
                        sample.totalUs = total;
                    }
                    break;
                }
            }
        } catch (Exception ignored) {
        }
        return sample;
    }

    private static int computePsiPct(PsiSample cur, PsiSample prev, long dtMs) {
        if (!cur.valid) return -1;
        int rounded;
        if (prev != null && prev.valid && dtMs > 0 && cur.totalUs >= prev.totalUs) {
            double pct = ((double) (cur.totalUs - prev.totalUs) / ((double) dtMs * 1000.0)) * 100.0;
            rounded = (int) Math.round(pct);
        } else {
            rounded = Math.round(cur.avg10);
        }
        return Math.max(0, Math.min(100, rounded));
    }

    private static String buildBootloaderLine(Method get) {
        List<String[]> kvs = new ArrayList<>();
        try {
            appendKvTokens(property(get, "ro.boot.boottime"), kvs);
            appendKvTokens(property(get, "ro.boot.boottime_extra"), kvs);
        } catch (Exception ignored) {
        }
        if (kvs.isEmpty()) return "BL N/A";
        String[] preferred = {"2BLE", "3BLE", "AVB", "KL", "DISP", "DRAM_TRAIN", "SW"};
        StringBuilder sb = new StringBuilder("BL");
        for (String key : preferred) {
            for (String[] kv : kvs) {
                if (key.equals(kv[0]) && !"0".equals(kv[1])) {
                    String tok = " " + kv[0] + ":" + kv[1];
                    if (sb.length() + tok.length() <= 35) {
                        sb.append(tok);
                    }
                    break;
                }
            }
        }
        if (sb.length() == 2) {
            for (String[] kv : kvs) {
                String tok = " " + kv[0] + ":" + kv[1];
                if (sb.length() + tok.length() <= 35) {
                    sb.append(tok);
                } else {
                    break;
                }
            }
        }
        return sb.toString();
    }

    private static void appendKvTokens(String raw, List<String[]> out) {
        if (raw == null || raw.isEmpty()) return;
        for (String item : raw.split(",")) {
            int idx = item.indexOf(':');
            if (idx > 0 && idx + 1 < item.length()) {
                out.add(new String[]{item.substring(0, idx), item.substring(idx + 1)});
            }
        }
    }

    private static void run(String[] args) throws Exception {
        boolean preview = args.length == 1 && "--preview".equals(args[0]);
        Method get = Class.forName("android.os.SystemProperties").getMethod("get", String.class);
        if (!preview && !animationActive(get)) {
            System.out.println("exit=animation_inactive");
            return;
        }

        DisplayTracker displayTracker = new DisplayTracker(get);
        AtomicReference<StageInfo> stageRef = new AtomicReference<>(
            new StageInfo(0, "WAITING EVENTS", "")
        );
        AtomicReference<Process> logcatProcRef = new AtomicReference<>();
        Thread eventReader = startEventReader(stageRef, logcatProcRef);

        SurfaceControl control = new SurfaceControl.Builder()
            .setName(NAME).setOpaque(true).setHidden(true).build();
        Bitmap bitmap = null;
        DisplayGeometry lastGeometry = null;
        Map<String, String> previousStates = new LinkedHashMap<>();
        String lastTransition = "WAITING TRANSITIONS";
        String bootloaderLine = buildBootloaderLine(get);
        PsiSample[] prevPsi = new PsiSample[3];
        long prevPsiMs = 0L;

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

                DisplayGeometry geometry = displayTracker.current();
                boolean geometryChanged = !geometry.sameTransform(lastGeometry);
                if (geometryChanged) {
                    String prefix = lastGeometry == null ? "layout=" : "layout_changed=";
                    System.out.println(prefix + geometry.label()
                        + " pos=" + Math.round(geometry.posX) + "," + Math.round(geometry.posY)
                        + " scale=" + String.format(Locale.ROOT, "%.3f", geometry.scale));
                }

                String transition = updateStatesAndDetectTransition(get, previousStates);
                if (transition != null) {
                    lastTransition = transition;
                }

                long nowMs = SystemClock.elapsedRealtime();
                PsiSample[] curPsi = new PsiSample[]{
                    readPsiFile("/proc/pressure/cpu"),
                    readPsiFile("/proc/pressure/io"),
                    readPsiFile("/proc/pressure/memory")
                };
                long dtMs = prevPsiMs > 0L ? (nowMs - prevPsiMs) : 0L;
                int cpuPct = computePsiPct(curPsi[0], prevPsi[0], dtMs);
                int ioPct = computePsiPct(curPsi[1], prevPsi[1], dtMs);
                int memPct = computePsiPct(curPsi[2], prevPsi[2], dtMs);
                prevPsi = curPsi;
                prevPsiMs = nowMs;

                draw(canvas, paint, get, preview, geometry, stageRef.get(), previousStates,
                     cpuPct, ioPct, memPct, bootloaderLine, lastTransition);
                submit(control, bitmap, first, geometryChanged ? geometry : null);

                if (first) {
                    System.out.println("first_frame_submitted_ms=" + SystemClock.elapsedRealtime());
                    first = false;
                }
                lastGeometry = geometry;
                Thread.sleep(250);
            }
            System.out.println("exit=time_limit");
        } finally {
            Process logcatProc = logcatProcRef.getAndSet(null);
            if (logcatProc != null) {
                logcatProc.destroyForcibly();
            }
            eventReader.interrupt();
            try (SurfaceControl.Transaction transaction = new SurfaceControl.Transaction()) {
                transaction.setVisibility(control, false).setBuffer(control, null).apply();
            } finally {
                control.release();
                if (bitmap != null) bitmap.recycle();
                System.out.println("released_ms=" + SystemClock.elapsedRealtime());
            }
        }
    }

    private static Thread startEventReader(
            final AtomicReference<StageInfo> stageRef,
            final AtomicReference<Process> procRef) {
        Thread thread = new Thread(new Runnable() {
            @Override
            public void run() {
                Process proc = null;
                try {
                    proc = new ProcessBuilder(
                        "/system/bin/logcat", "-b", "events", "-v", "tag", "-s",
                        "boot_progress_start:I",
                        "boot_progress_preload_start:I",
                        "boot_progress_preload_end:I",
                        "boot_progress_system_run:I",
                        "boot_progress_pms_start:I",
                        "boot_progress_pms_system_scan_start:I",
                        "boot_progress_pms_data_scan_start:I",
                        "boot_progress_pms_scan_end:I",
                        "boot_progress_pms_ready:I",
                        "boot_progress_ams_ready:I",
                        "boot_progress_enable_screen:I",
                        "sf_stop_bootanim:I",
                        "wm_boot_animation_done:I"
                    ).redirectErrorStream(true).start();
                    procRef.set(proc);
                    try (BufferedReader reader = new BufferedReader(
                            new InputStreamReader(proc.getInputStream(), StandardCharsets.UTF_8))) {
                        String line;
                        while (!Thread.currentThread().isInterrupted() && (line = reader.readLine()) != null) {
                            Matcher m = EVENT_PATTERN.matcher(line);
                            if (m.matches()) {
                                StageInfo next = mapStage(m.group(1), m.group(2));
                                if (next != null) {
                                    stageRef.set(next);
                                }
                            }
                        }
                    }
                } catch (Exception ignored) {
                } finally {
                    if (proc != null) proc.destroyForcibly();
                }
            }
        }, "bootscope-events");
        thread.setDaemon(true);
        thread.start();
        return thread;
    }

    private static StageInfo mapStage(String tag, String rawMs) {
        switch (tag) {
            case "boot_progress_start":
                return new StageInfo(1, "START", rawMs);
            case "boot_progress_preload_start":
                return new StageInfo(2, "PRELOAD", rawMs);
            case "boot_progress_preload_end":
                return new StageInfo(3, "PRELOADED", rawMs);
            case "boot_progress_system_run":
                return new StageInfo(4, "SYSTEM_RUN", rawMs);
            case "boot_progress_pms_start":
                return new StageInfo(5, "PMS_START", rawMs);
            case "boot_progress_pms_system_scan_start":
                return new StageInfo(5, "PMS_SYS_SCAN", rawMs);
            case "boot_progress_pms_data_scan_start":
                return new StageInfo(5, "PMS_DATA_SCAN", rawMs);
            case "boot_progress_pms_scan_end":
                return new StageInfo(5, "PMS_SCANNED", rawMs);
            case "boot_progress_pms_ready":
                return new StageInfo(6, "PMS_READY", rawMs);
            case "boot_progress_ams_ready":
                return new StageInfo(7, "AMS_READY", rawMs);
            case "boot_progress_enable_screen":
                return new StageInfo(8, "ENABLE_SCREEN", rawMs);
            case "sf_stop_bootanim":
                return new StageInfo(8, "STOP_BOOTANIM", rawMs);
            case "wm_boot_animation_done":
                return new StageInfo(8, "ANIM_DONE", rawMs);
            default:
                return null;
        }
    }

    private static String updateStatesAndDetectTransition(
            Method get, Map<String, String> previousStates) throws Exception {
        String latest = null;
        double nowSec = SystemClock.elapsedRealtime() / 1000d;
        boolean initial = previousStates.isEmpty();
        for (String name : WATCHED_SERVICES) {
            String next = shortState(state(get, name));
            String prev = previousStates.put(name, next);
            if (!initial && prev != null && !prev.equals(next)) {
                latest = String.format(
                    Locale.ROOT, "+%.2fS %s %s>%s",
                    nowSec, shortServiceName(name), prev, next
                );
            }
        }
        return latest;
    }

    private static void submit(
            SurfaceControl control, Bitmap bitmap, boolean first, DisplayGeometry geometry) {
        Bitmap hardware = bitmap.copy(Bitmap.Config.HARDWARE, false);
        if (hardware == null) throw new IllegalStateException("Could not allocate frame buffer");
        try (HardwareBuffer buffer = hardware.getHardwareBuffer();
                SurfaceControl.Transaction transaction = new SurfaceControl.Transaction()) {
            if (geometry != null) {
                transaction.setPosition(control, geometry.posX, geometry.posY)
                    .setScale(control, geometry.scale, geometry.scale);
            }
            if (first) {
                transaction.setLayer(control, LAYER).setVisibility(control, true);
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

    private static String shortState(String value) {
        switch (value) {
            case "running":
                return "RUN";
            case "stopped":
                return "STOP";
            case "restarting":
                return "RST";
            case "stopping":
                return "STPG";
            default:
                return "-";
        }
    }

    private static String shortServiceName(String name) {
        if ("surfaceflinger".equals(name)) return "SF";
        if ("bootanim".equals(name)) return "ANIM";
        if ("zygote".equals(name)) return "ZYG";
        if ("apexd".equals(name)) return "APEX";
        if ("artd".equals(name)) return "ARTD";
        if ("wpa_supplicant".equals(name)) return "WPA";
        return name;
    }

    private static void draw(
            Canvas canvas, Paint paint, Method get, boolean preview,
            DisplayGeometry geometry, StageInfo stage,
            Map<String, String> states, int cpuPct, int ioPct, int memPct,
            String bootloaderLine, String lastTransition) throws Exception {
        String uptime = String.format(Locale.ROOT, "%.2fS", SystemClock.elapsedRealtime() / 1000d);
        String modeLabel = geometry.label();

        canvas.drawColor(0xff101b24);
        paint.setColor(0xff7be0ca);
        canvas.drawRect(0, 0, 8, HEIGHT, paint);

        BootFont.draw(canvas, paint, preview ? "BOOTSCOPE / PREVIEW" : "BOOTSCOPE / BOOT", 24, 24, 3);

        paint.setColor(0xff9ec3d6);
        int modeX = Math.max(384, WIDTH - 24 - modeLabel.length() * 18);
        BootFont.draw(canvas, paint, modeLabel, modeX, 24, 3);

        paint.setColor(0xffedf4f7);
        String headerMeta = String.format(Locale.ROOT, "UPTIME %-8s  STAGE %d/%d", uptime, stage.step, STAGE_TOTAL);
        BootFont.draw(canvas, paint, headerMeta, 24, 66, 3);

        int barLeft = 24;
        int barTop = 98;
        int barRight = WIDTH - 24;
        int barBottom = 110;
        paint.setColor(0xff1e3242);
        canvas.drawRect(barLeft, barTop, barRight, barBottom, paint);
        if (stage.step > 0) {
            int filled = barLeft + Math.round((barRight - barLeft) * (stage.step / (float) STAGE_TOTAL));
            paint.setColor(0xff7be0ca);
            canvas.drawRect(barLeft, barTop, filled, barBottom, paint);
        }

        paint.setColor(0xff7be0ca);
        String stageLine = stage.rawMs.isEmpty()
            ? stage.label
            : stage.label + "  " + stage.rawMs + "MS";
        BootFont.draw(canvas, paint, stageLine, 24, 126, 3);

        paint.setColor(0xffedf4f7);
        String row1 = String.format(
            Locale.ROOT, "SF:%-4s ANIM:%-4s ZYG:%-4s",
            states.get("surfaceflinger"), states.get("bootanim"), states.get("zygote")
        );
        String row2 = String.format(
            Locale.ROOT, "APEX:%-4s ARTD:%-4s WPA:%-4s",
            states.get("apexd"), states.get("artd"), states.get("wpa_supplicant")
        );
        BootFont.draw(canvas, paint, row1, 24, 156, 3);
        BootFont.draw(canvas, paint, row2, 24, 186, 3);

        paint.setColor(0xff7be0ca);
        String psiLine = (cpuPct >= 0 && ioPct >= 0 && memPct >= 0)
            ? String.format(Locale.ROOT, "PSI CPU:%3d%% IO:%3d%% MEM:%3d%%", cpuPct, ioPct, memPct)
            : "PSI N/A";
        BootFont.draw(canvas, paint, psiLine, 24, 218, 3);

        paint.setColor(0xff9ec3d6);
        BootFont.draw(canvas, paint, bootloaderLine, 24, 248, 3);

        String bc = property(get, "sys.boot_completed");
        String animExit = property(get, "service.bootanim.exit");
        String bcStr = (bc == null || bc.isEmpty()) ? "0" : bc;
        String animStr = (animExit == null || animExit.isEmpty()) ? "0" : animExit;
        String footer = "WAITING TRANSITIONS".equals(lastTransition)
            ? String.format(Locale.ROOT, "BC=%s ANIM_EXIT=%s", bcStr, animStr)
            : "LAST " + lastTransition;
        if (footer.length() > 35) footer = footer.substring(0, 35);
        BootFont.draw(canvas, paint, footer, 24, 280, 3);
    }

    private static Method findMethod(Class<?> cls, String name, Class<?>... params) {
        try {
            return cls.getMethod(name, params);
        } catch (Exception ignored) {
            return null;
        }
    }

    private static Method findMethod(String className, String name, Class<?>... params) {
        try {
            return findMethod(Class.forName(className), name, params);
        } catch (Exception ignored) {
            return null;
        }
    }

    private static int readIntField(Object obj, String name) {
        try {
            Field f = obj.getClass().getField(name);
            return f.getInt(obj);
        } catch (Exception ignored) {
            return 0;
        }
    }

    private static float readFloatField(Object obj, String name) {
        try {
            Field f = obj.getClass().getField(name);
            return f.getFloat(obj);
        } catch (Exception ignored) {
            return 0f;
        }
    }
}
