package bootscope;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.hardware.HardwareBuffer;
import android.os.SystemClock;
import android.view.DisplayCutout;
import android.view.SurfaceControl;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.InputStreamReader;
import java.lang.reflect.Array;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.atomic.AtomicReference;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public final class DisplayProbe {
    private static final String NAME = "BootScopeDisplay";
    private static final int WIDTH = 760;
    private static final int HEIGHT = 632;
    private static final int LAYER = 0x40000001;
    private static final int STAGE_TOTAL = 8;
    private static final int MAX_TAIL_ROWS = 8;
    private static final int TAIL_LINE_CHARS = 59;
    private static final String FONT_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./:-_=+@>%?";
    private static final String USER_RULES_PATH = "/data/adb/bootscope/rules.conf";
    private static final String MODULE_RULES_PATH = "/data/adb/modules/bootscope/rules.conf";
    private static final String EARLY_RULES_PATH = "/metadata/vold/ksu/bootscope_rules.conf";

    private static final String[] WATCHED_SERVICES = {
        "surfaceflinger", "bootanim", "zygote", "apexd", "artd", "wpa_supplicant"
    };
    private static final String[] RESIDENT_DAEMONS = {
        "ueventd", "logd", "servicemanager", "hwservicemanager", "vndservicemanager",
        "vold", "netd", "surfaceflinger", "bootanim", "bootscope_early",
        "zygote", "zygote64", "webview_zygote", "adbd", "lmkd", "tombstoned",
        "traced", "traced_probes", "statsd", "keystore2", "credstore",
        "incidentd", "storaged", "audioserver", "cameraserver", "mediaserver",
        "drmserver", "thermalservice", "apexd", "artd", "wpa_supplicant"
    };
    private static final Pattern DISPLAY_ID_PATTERN = Pattern.compile("Display\\s+(\\d+)\\b");
    private static final Pattern MODE_PROP_PATTERN = Pattern.compile("(\\d+)x(\\d+)(?:@(\\d+))?");
    private static final Pattern EVENT_PATTERN = Pattern.compile(
        "^[IWEF]/([a-z0-9_]+):\\s*(\\d+)\\s*$"
    );
    private static final Pattern MONO_ERR_PATTERN = Pattern.compile(
        "^\\s*(\\d+\\.\\d+)\\s+\\d+\\s+\\d+\\s+([EF])\\s+([^:]+):(.*)$"
    );
    private static final Pattern DMESG_ANY_PATTERN = Pattern.compile(
        "^\\[\\s*(\\d+\\.\\d+)\\]\\s+(.*)$"
    );

    private enum TailKind {
        INFO, STAGE, ERROR, MODULE_ERROR
    }

    private enum NodeState {
        PENDING, RUNNING, DONE
    }

    private static final class RuleSet {
        final List<String> ignoreCmd = new ArrayList<>();
        final List<String> ignoreSvc = new ArrayList<>();
        final List<String> ignoreTag = new ArrayList<>();
        final List<String> moduleKey = new ArrayList<>();
        final List<String> redactKey = new ArrayList<>();
        String source = "BUILTIN";
        int count = 0;

        static RuleSet load() {
            RuleSet r = new RuleSet();
            r.moduleKey.add("bootscope");
            Collections.addAll(
                r.redactKey,
                "imei", "imsi", "iccid", "bssid", "ssid=", "password",
                "secret", "token=", "cookie", "serialno=", "android_id",
                "mac=", "macaddr"
            );
            r.count = r.moduleKey.size() + r.redactKey.size();

            String[][] candidates = {
                {USER_RULES_PATH, "USR"},
                {MODULE_RULES_PATH, "DEF"},
                {EARLY_RULES_PATH, "DEF"}
            };
            File chosen = null;
            String label = "BUILTIN";
            for (String[] pair : candidates) {
                File f = new File(pair[0]);
                if (f.isFile() && f.canRead()) {
                    chosen = f;
                    label = pair[1];
                    break;
                }
            }
            if (chosen == null) return r;

            RuleSet parsed = new RuleSet();
            parsed.source = label;
            try (BufferedReader reader = new BufferedReader(new FileReader(chosen))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    String s = line.trim();
                    if (s.isEmpty() || s.startsWith("#")) continue;
                    int eq = s.indexOf('=');
                    if (eq <= 0 || eq + 1 >= s.length()) continue;
                    String key = s.substring(0, eq).trim().toLowerCase(Locale.ROOT);
                    String val = s.substring(eq + 1).trim().toLowerCase(Locale.ROOT);
                    if (val.isEmpty()) continue;
                    if ("ignore_cmd".equals(key)) parsed.ignoreCmd.add(val);
                    else if ("ignore_svc".equals(key)) parsed.ignoreSvc.add(val);
                    else if ("ignore_tag".equals(key)) parsed.ignoreTag.add(val);
                    else if ("module_key".equals(key)) parsed.moduleKey.add(val);
                    else if ("redact_key".equals(key)) parsed.redactKey.add(val);
                }
            } catch (Exception ignored) {
                return r;
            }
            if (parsed.moduleKey.isEmpty()) parsed.moduleKey.addAll(r.moduleKey);
            if (parsed.redactKey.isEmpty()) parsed.redactKey.addAll(r.redactKey);
            parsed.count = parsed.ignoreCmd.size() + parsed.ignoreSvc.size()
                + parsed.ignoreTag.size() + parsed.moduleKey.size() + parsed.redactKey.size();
            return parsed;
        }

        boolean hasSensitiveMarker(String raw) {
            String low = raw.toLowerCase(Locale.ROOT);
            for (String kw : redactKey) {
                if (low.contains(kw)) return true;
            }
            return false;
        }

        boolean matchesModuleKey(String raw) {
            String low = raw.toLowerCase(Locale.ROOT);
            for (String kw : moduleKey) {
                if (low.contains(kw)) return true;
            }
            return false;
        }

        boolean matchesIgnoreCmd(String fullCmd) {
            String low = fullCmd.toLowerCase(Locale.ROOT);
            for (String prefix : ignoreCmd) {
                if (low.equals(prefix)) return true;
                if (low.length() > prefix.length() && low.startsWith(prefix)) {
                    char back = prefix.charAt(prefix.length() - 1);
                    char next = low.charAt(prefix.length());
                    if (back == '-' || back == '_' || back == '/' || next == ' ') {
                        return true;
                    }
                }
            }
            return false;
        }

        boolean matchesIgnoreSvc(String svc) {
            String low = svc.toLowerCase(Locale.ROOT);
            for (String s : ignoreSvc) {
                if (low.equals(s)) return true;
            }
            return false;
        }

        boolean matchesIgnoreTag(String tag) {
            String low = tag.toLowerCase(Locale.ROOT);
            for (String s : ignoreTag) {
                if (low.equals(s)) return true;
            }
            return false;
        }
    }

    private static final class PhaseNode {
        final String id;
        NodeState state = NodeState.PENDING;
        long startMs = -1L;
        long endMs = -1L;
        String detail = "";

        PhaseNode(String id) {
            this.id = id;
        }

        PhaseNode copy() {
            PhaseNode p = new PhaseNode(id);
            p.state = state;
            p.startMs = startMs;
            p.endMs = endMs;
            p.detail = detail;
            return p;
        }
    }

    private static final class PhaseTracker {
        private final PhaseNode[] phases = new PhaseNode[]{
            new PhaseNode("BL"), new PhaseNode("INIT"), new PhaseNode("ZYG"),
            new PhaseNode("PRE"), new PhaseNode("PMS"), new PhaseNode("AMS")
        };
        private final Map<String, Long> activeSvcs = new LinkedHashMap<>();
        private String lastInitAction = "";
        private String maxInitSvc = "";
        private long maxInitDurMs = 0L;
        private boolean hasBlStart = false;
        private boolean hasBlEnd = false;
        private boolean hasBlLoad = false;
        private boolean hasFreq = false;
        private long blStart = 0L;
        private long blEnd = 0L;
        private long blLoad = 0L;
        private long mpmFreq = 0L;

        synchronized void onQcomKpiLine(String raw) {
            int idx = raw.indexOf("KPI: ");
            if (idx < 0) return;
            String s = raw.substring(idx + 5);
            int eq = s.indexOf('=');
            if (eq <= 0 || eq + 1 >= s.length()) return;
            String key = s.substring(0, eq).trim();
            long val;
            try {
                val = Long.parseLong(s.substring(eq + 1).trim());
            } catch (NumberFormatException ignored) {
                return;
            }
            if ("Bootloader start count".equals(key)) {
                blStart = val;
                hasBlStart = true;
            } else if ("Bootloader end count".equals(key)) {
                blEnd = val;
                hasBlEnd = true;
            } else if ("Bootloader load kernel count".equals(key)) {
                blLoad = val;
                hasBlLoad = true;
            } else if ("Kernel MPM Clock frequency".equals(key)) {
                mpmFreq = val;
                hasFreq = true;
            } else {
                return;
            }
            if (!"BOOTLOADER TIMING N/A".equals(phases[0].detail)
                    && !phases[0].detail.startsWith("QCOM KPI")) {
                return;
            }
            if (!hasBlStart || !hasBlEnd || blEnd < blStart) return;
            long totalTicks = blEnd - blStart;
            if (hasFreq && mpmFreq > 0L) {
                long totalMs = Math.round(((double) totalTicks * 1000.0d) / (double) mpmFreq);
                phases[0].startMs = 0L;
                phases[0].endMs = totalMs;
                if (hasBlLoad && blLoad >= blStart && blLoad <= blEnd) {
                    long preMs = Math.round(((double) (blLoad - blStart) * 1000.0d) / (double) mpmFreq);
                    long klMs = Math.round(((double) (blEnd - blLoad) * 1000.0d) / (double) mpmFreq);
                    phases[0].detail = String.format(Locale.ROOT, "QCOM KPI PRE:%dMS KL:%dMS", preMs, klMs);
                } else {
                    phases[0].detail = String.format(Locale.ROOT, "QCOM KPI BL:%dMS F:%d", totalMs, mpmFreq);
                }
            } else {
                phases[0].detail = String.format(Locale.ROOT, "QCOM KPI TICKS:%d", totalTicks);
            }
        }

        synchronized void init(Method get, String blDetail) {
            phases[0].state = NodeState.DONE;
            phases[0].startMs = -1L;
            phases[0].endMs = -1L;
            phases[0].detail = blDetail;

            phases[1].state = NodeState.RUNNING;
            phases[1].startMs = 0L;
            phases[1].detail = "EARLY-INIT -> ZYGOTE";

            phases[2].detail = "ZYGOTE -> SYSTEM_SERVER";
            phases[3].detail = "PRELOAD CLASSES";
            phases[4].detail = "SYSTEM / DATA PKG SCAN";
            phases[5].detail = "AMS_READY -> SCREEN";

            try {
                String zygStr = property(get, "ro.boottime.zygote");
                if (zygStr != null && !zygStr.isEmpty()) {
                    long zygNs = Long.parseLong(zygStr);
                    if (zygNs > 0L) {
                        long zygMs = zygNs / 1000000L;
                        phases[1].state = NodeState.DONE;
                        phases[1].endMs = zygMs;
                        if (phases[2].state == NodeState.PENDING) {
                            phases[2].state = NodeState.RUNNING;
                            phases[2].startMs = zygMs;
                        }
                    }
                }
            } catch (Exception ignored) {
            }
        }

        synchronized void onServiceStart(String svc, long tsMs) {
            if (!isResidentDaemon(svc)) {
                if (activeSvcs.size() >= 32) {
                    String firstKey = activeSvcs.keySet().iterator().next();
                    activeSvcs.remove(firstKey);
                }
                activeSvcs.put(svc, tsMs);
            }
        }

        synchronized void onServiceExit(String svc, long tsMs, long durMs) {
            activeSvcs.remove(svc);
            boolean inInit = (phases[1].state == NodeState.RUNNING)
                || (phases[1].endMs > 0L && tsMs <= phases[1].endMs + 500L);
            if (inInit && durMs > maxInitDurMs && !isResidentDaemon(svc)) {
                String cleanSvc = svc;
                if (cleanSvc.startsWith("exec ")) {
                    int lp = cleanSvc.indexOf('(');
                    int rp = cleanSvc.lastIndexOf(')');
                    if (lp >= 0 && rp > lp + 1) {
                        cleanSvc = cleanSvc.substring(lp + 1, rp);
                        int sp = cleanSvc.indexOf(' ');
                        if (sp > 0) cleanSvc = cleanSvc.substring(0, sp);
                        int sl = cleanSvc.lastIndexOf('/');
                        if (sl >= 0) cleanSvc = cleanSvc.substring(sl + 1);
                    }
                }
                maxInitDurMs = durMs;
                maxInitSvc = cleanSvc;
            }
        }

        synchronized void onInitAction(String act) {
            lastInitAction = act;
        }

        synchronized void onZygoteRunning(long nowMs) {
            if (phases[1].state == NodeState.RUNNING) {
                phases[1].state = NodeState.DONE;
                phases[1].endMs = nowMs;
                if (phases[2].state == NodeState.PENDING) {
                    phases[2].state = NodeState.RUNNING;
                    phases[2].startMs = nowMs;
                }
            }
        }

        synchronized void onFrameworkTag(String tag, long tsMs, String label) {
            if ("boot_progress_start".equals(tag)) {
                if (phases[1].state != NodeState.DONE) {
                    phases[1].state = NodeState.DONE;
                    if (phases[1].endMs < 0L) phases[1].endMs = tsMs;
                }
                if (phases[2].startMs < 0L) phases[2].startMs = 0L;
                phases[2].state = NodeState.DONE;
                phases[2].endMs = tsMs;
                phases[2].detail = "SYSTEM_SERVER STARTED";
                if (phases[3].state == NodeState.PENDING) {
                    phases[3].state = NodeState.RUNNING;
                    phases[3].startMs = tsMs;
                    phases[3].detail = "WAITING PRELOAD";
                }
            } else if ("boot_progress_preload_start".equals(tag)) {
                phases[3].state = NodeState.RUNNING;
                phases[3].startMs = tsMs;
                phases[3].detail = "PRELOADING CLASSES";
            } else if ("boot_progress_preload_end".equals(tag)) {
                if (phases[3].startMs < 0L) phases[3].startMs = tsMs;
                phases[3].state = NodeState.DONE;
                phases[3].endMs = tsMs;
                phases[3].detail = "PRELOADED";
                if (phases[4].state == NodeState.PENDING) {
                    phases[4].state = NodeState.RUNNING;
                    phases[4].startMs = tsMs;
                    phases[4].detail = "WAITING SYSTEM_RUN";
                }
            } else if ("boot_progress_system_run".equals(tag)) {
                if (phases[3].state != NodeState.DONE) {
                    if (phases[3].startMs < 0L) phases[3].startMs = tsMs;
                    phases[3].state = NodeState.DONE;
                    phases[3].endMs = tsMs;
                }
                phases[4].state = NodeState.RUNNING;
                if (phases[4].startMs < 0L) phases[4].startMs = tsMs;
                phases[4].detail = "SYSTEM_RUN";
            } else if ("boot_progress_pms_start".equals(tag)) {
                phases[4].state = NodeState.RUNNING;
                if (phases[4].startMs < 0L) phases[4].startMs = tsMs;
                phases[4].detail = "PMS_START";
            } else if ("boot_progress_pms_system_scan_start".equals(tag)) {
                phases[4].state = NodeState.RUNNING;
                if (phases[4].startMs < 0L) phases[4].startMs = tsMs;
                phases[4].detail = "SYSTEM_PKG_SCAN";
            } else if ("boot_progress_pms_data_scan_start".equals(tag)) {
                phases[4].state = NodeState.RUNNING;
                if (phases[4].startMs < 0L) phases[4].startMs = tsMs;
                phases[4].detail = "DATA_PKG_SCAN";
            } else if ("boot_progress_pms_scan_end".equals(tag)) {
                phases[4].detail = "SCAN_END";
            } else if ("boot_progress_pms_ready".equals(tag)) {
                if (phases[4].startMs < 0L) phases[4].startMs = tsMs;
                phases[4].state = NodeState.DONE;
                phases[4].endMs = tsMs;
                phases[4].detail = "PMS_READY";
                if (phases[5].state == NodeState.PENDING) {
                    phases[5].state = NodeState.RUNNING;
                    phases[5].startMs = tsMs;
                    phases[5].detail = "WAITING AMS_READY";
                }
            } else if ("boot_progress_ams_ready".equals(tag)) {
                if (phases[4].state != NodeState.DONE) {
                    if (phases[4].startMs < 0L) phases[4].startMs = tsMs;
                    phases[4].state = NodeState.DONE;
                    phases[4].endMs = tsMs;
                }
                phases[5].state = NodeState.RUNNING;
                if (phases[5].startMs < 0L) phases[5].startMs = tsMs;
                phases[5].detail = "AMS_READY -> SCREEN";
            } else if ("boot_progress_enable_screen".equals(tag)
                    || "sf_stop_bootanim".equals(tag)
                    || "wm_boot_animation_done".equals(tag)) {
                if (phases[5].startMs < 0L) phases[5].startMs = tsMs;
                phases[5].state = NodeState.DONE;
                phases[5].endMs = tsMs;
                phases[5].detail = label;
            }
        }

        synchronized PhaseSnapshot snapshot(long nowMs) {
            if (!lastInitAction.isEmpty() || !maxInitSvc.isEmpty()) {
                StringBuilder sb = new StringBuilder();
                if (!lastInitAction.isEmpty()) {
                    String actLabel = lastInitAction;
                    if ("load_persist_props_action".equals(actLabel)) actLabel = "PERSIST_PROPS";
                    else if ("firmware_mounts_complete".equals(actLabel)) actLabel = "FW_MOUNTS";
                    else actLabel = actLabel.toUpperCase(Locale.ROOT);
                    sb.append("ACT:").append(actLabel);
                }
                if (!maxInitSvc.isEmpty()) {
                    if (sb.length() > 0) sb.append(' ');
                    sb.append(String.format(Locale.ROOT, "MAX:%s %.2fS", maxInitSvc, maxInitDurMs / 1000.0d));
                }
                phases[1].detail = sb.toString();
            }
            PhaseNode[] copy = new PhaseNode[phases.length];
            for (int i = 0; i < phases.length; i++) copy[i] = phases[i].copy();

            String waitSvc = "";
            long waitDurMs = 0L;
            for (Map.Entry<String, Long> e : activeSvcs.entrySet()) {
                long d = nowMs - e.getValue();
                if (d > waitDurMs) {
                    waitDurMs = d;
                    waitSvc = e.getKey();
                }
            }
            return new PhaseSnapshot(
                copy, waitSvc, waitDurMs, maxInitSvc, maxInitDurMs, lastInitAction, activeSvcs.size()
            );
        }
    }

    private static final class PhaseSnapshot {
        final PhaseNode[] phases;
        final String waitSvc;
        final long waitDurMs;
        final String maxInitSvc;
        final long maxInitDurMs;
        final String lastInitAction;
        final int activeCnt;

        PhaseSnapshot(
                PhaseNode[] phases, String waitSvc, long waitDurMs,
                String maxInitSvc, long maxInitDurMs, String lastInitAction, int activeCnt) {
            this.phases = phases;
            this.waitSvc = waitSvc;
            this.waitDurMs = waitDurMs;
            this.maxInitSvc = maxInitSvc;
            this.maxInitDurMs = maxInitDurMs;
            this.lastInitAction = lastInitAction;
            this.activeCnt = activeCnt;
        }
    }

    private static final class DeviceInfo {
        final String line1;
        final String line2;

        DeviceInfo(String line1, String line2) {
            this.line1 = line1;
            this.line2 = line2;
        }
    }

    private static final class TailEntry {
        long tsMs;
        long seq;
        final String source;
        final String group;
        String text;
        TailKind kind;

        TailEntry(long tsMs, long seq, String source, String group, String text, TailKind kind) {
            this.tsMs = tsMs;
            this.seq = seq;
            this.source = source;
            this.group = group;
            this.text = text;
            this.kind = kind;
        }
    }

    private static final class TailBuffer {
        private final List<TailEntry> entries = new ArrayList<>();
        private final RuleSet rules;
        private long nextSeq = 1L;

        TailBuffer(RuleSet rules) {
            this.rules = rules;
        }

        synchronized void push(long tsMs, String source, TailKind kind, String rawText) {
            push(tsMs, source, kind, rawText, "");
        }

        synchronized void push(long tsMs, String source, TailKind kind, String rawText, String group) {
            String text = sanitizeTailText(rules, rawText, TAIL_LINE_CHARS - 8);
            if (text.isEmpty()) return;
            for (TailEntry existing : entries) {
                boolean sameMsg = existing.source.equals(source) && existing.text.equals(text);
                boolean sameErrTag = !group.isEmpty() && existing.source.equals(source) && existing.group.equals(group);
                if (sameMsg || sameErrTag) {
                    if (tsMs >= existing.tsMs) {
                        existing.tsMs = tsMs;
                        existing.text = text;
                        existing.kind = kind;
                        existing.seq = nextSeq++;
                    }
                    sortAndTrim();
                    return;
                }
            }
            entries.add(new TailEntry(tsMs, nextSeq++, source, group, text, kind));
            sortAndTrim();
        }

        private void sortAndTrim() {
            Collections.sort(entries, new Comparator<TailEntry>() {
                @Override
                public int compare(TailEntry a, TailEntry b) {
                    int cmp = Long.compare(a.tsMs, b.tsMs);
                    return cmp != 0 ? cmp : Long.compare(a.seq, b.seq);
                }
            });
            while (entries.size() > 16) {
                entries.remove(0);
            }
        }

        synchronized List<TailEntry> visibleSnapshot() {
            int size = entries.size();
            int start = Math.max(0, size - MAX_TAIL_ROWS);
            List<TailEntry> out = new ArrayList<>(size - start);
            for (int i = start; i < size; i++) {
                TailEntry e = entries.get(i);
                out.add(new TailEntry(e.tsMs, e.seq, e.source, e.group, e.text, e.kind));
            }
            return out;
        }
    }

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
            float marginX = Math.max(20f, Math.round(width * 0.03f));
            float targetWidth = Math.min(width - marginX * 2f, Math.round(minDim * 0.86f));
            this.scale = Math.max(0.5f, targetWidth / (float) WIDTH);
            this.posX = Math.round((width - targetWidth) * 0.5f);
            float minTop = Math.round(height * 0.055f);
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

    private static boolean isResidentDaemon(String svc) {
        if (svc.startsWith("vendor.") || svc.startsWith("android.hardware.")) return true;
        for (String d : RESIDENT_DAEMONS) {
            if (svc.equals(d)) return true;
        }
        return false;
    }

    private static boolean isMajorInitAction(String act) {
        return "early-init".equals(act) || "init".equals(act) || "late-fs".equals(act)
            || "post-fs".equals(act) || "post-fs-data".equals(act)
            || "load_persist_props_action".equals(act) || "zygote-start".equals(act)
            || "firmware_mounts_complete".equals(act) || "boot".equals(act);
    }

    private static int readMemTotalGb() {
        try (BufferedReader reader = new BufferedReader(new FileReader("/proc/meminfo"))) {
            String line;
            while ((line = reader.readLine()) != null) {
                if (line.startsWith("MemTotal:")) {
                    String[] parts = line.trim().split("\\s+");
                    if (parts.length >= 2) {
                        long kb = Long.parseLong(parts[1]);
                        int gb = (int) Math.round(kb / 1048576.0d);
                        return gb > 0 ? gb : 1;
                    }
                    break;
                }
            }
        } catch (Exception ignored) {
        }
        return 0;
    }

    private static DeviceInfo collectDeviceInfo(Method get, RuleSet rules) {
        String model = "";
        String dev = "";
        String rel = "";
        String sdk = "";
        String abi = "";
        String slot = "";
        String vbs = "";
        try {
            model = property(get, "ro.product.model");
            if (model == null || model.isEmpty()) model = property(get, "ro.product.vendor.model");
            dev = property(get, "ro.product.device");
            if (dev == null || dev.isEmpty()) dev = property(get, "ro.hardware");
            rel = property(get, "ro.build.version.release");
            sdk = property(get, "ro.build.version.sdk");
            abi = property(get, "ro.product.cpu.abi");
            slot = property(get, "ro.boot.slot_suffix");
            vbs = property(get, "ro.boot.verifiedbootstate");
        } catch (Exception ignored) {
        }
        if (model == null) model = "";
        if (dev == null) dev = "";
        if (rel == null) rel = "";
        if (sdk == null) sdk = "";
        if (abi == null) abi = "";
        if (slot == null) slot = "";
        if (vbs == null) vbs = "";
        if (slot.startsWith("_")) slot = slot.substring(1);

        StringBuilder l1 = new StringBuilder("DEV ");
        l1.append(model.isEmpty() ? "ANDROID" : model);
        if (!dev.isEmpty() && !dev.equalsIgnoreCase(model)) {
            l1.append(" / ").append(dev);
        }
        if (!rel.isEmpty() || !sdk.isEmpty()) {
            l1.append("  ");
            if (!rel.isEmpty()) l1.append("A").append(rel);
            if (!rel.isEmpty() && !sdk.isEmpty()) l1.append("/");
            if (!sdk.isEmpty()) l1.append("SDK").append(sdk);
        }

        int cores = Runtime.getRuntime().availableProcessors();
        int ramGb = readMemTotalGb();
        String krn = System.getProperty("os.version", "");
        if (krn != null) {
            int dash = krn.indexOf('-');
            if (dash > 0) krn = krn.substring(0, dash);
        } else {
            krn = "";
        }

        StringBuilder l2 = new StringBuilder(abi.isEmpty() ? "ARM64" : abi);
        if (cores > 0) l2.append(' ').append(cores).append('C');
        if (ramGb > 0) l2.append(' ').append(ramGb).append('G');
        if (!krn.isEmpty()) l2.append("  KRN ").append(krn);
        if (!slot.isEmpty()) l2.append("  SLOT:").append(slot);
        if (!vbs.isEmpty()) l2.append("  VB:").append(vbs);

        String s1 = sanitizeTailText(rules, l1.toString(), TAIL_LINE_CHARS);
        String s2 = sanitizeTailText(rules, l2.toString(), TAIL_LINE_CHARS);
        return new DeviceInfo(
            s1.isEmpty() ? "DEV UNKNOWN" : s1,
            s2.isEmpty() ? "HW INFO N/A" : s2
        );
    }

    private static String buildBootloaderLine(Method get) {
        List<String[]> kvs = new ArrayList<>();
        try {
            appendKvTokens(property(get, "ro.boot.boottime"), kvs);
            appendKvTokens(property(get, "ro.boot.boottime_extra"), kvs);
        } catch (Exception ignored) {
        }
        if (kvs.isEmpty()) {
            return "BOOTLOADER TIMING N/A";
        }
        String[] preferred = {"2BLE", "3BLE", "AVB", "KL", "DISP", "DRAM_TRAIN", "FWL"};
        StringBuilder sb = new StringBuilder();
        for (String key : preferred) {
            for (String[] kv : kvs) {
                if (key.equals(kv[0]) && !"0".equals(kv[1])) {
                    String tok = (sb.length() == 0 ? "" : " ") + kv[0] + ":" + kv[1];
                    if (sb.length() + tok.length() <= 42) {
                        sb.append(tok);
                    }
                    break;
                }
            }
        }
        if (sb.length() == 0) {
            for (String[] kv : kvs) {
                String tok = (sb.length() == 0 ? "" : " ") + kv[0] + ":" + kv[1];
                if (sb.length() + tok.length() <= 42) {
                    sb.append(tok);
                } else {
                    break;
                }
            }
        }
        return sb.length() == 0 ? "BOOTLOADER TIMING N/A" : sb.toString();
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

        RuleSet rules = RuleSet.load();
        DeviceInfo devInfo = collectDeviceInfo(get, rules);
        String bootloaderLine = buildBootloaderLine(get);

        DisplayTracker displayTracker = new DisplayTracker(get);
        AtomicReference<StageInfo> stageRef = new AtomicReference<>(
            new StageInfo(0, "WAITING EVENTS", "")
        );
        PhaseTracker phaseTracker = new PhaseTracker();
        phaseTracker.init(get, bootloaderLine);
        TailBuffer tailBuffer = new TailBuffer(rules);
        AtomicReference<Process> logcatProcRef = new AtomicReference<>();
        AtomicReference<Process> errProcRef = new AtomicReference<>();
        Thread eventReader = startEventReader(stageRef, phaseTracker, tailBuffer, logcatProcRef);
        Thread errReader = startErrReader(rules, tailBuffer, errProcRef);
        Thread dmesgReader = startDmesgInitReader(rules, phaseTracker, tailBuffer);

        SurfaceControl control = new SurfaceControl.Builder()
            .setName(NAME).setOpaque(true).setHidden(true).build();
        Bitmap bitmap = null;
        DisplayGeometry lastGeometry = null;
        Map<String, String> previousStates = new LinkedHashMap<>();
        String lastTransition = "WAITING TRANSITIONS";
        PsiSample[] prevPsi = new PsiSample[3];
        long prevPsiMs = 0L;

        try {
            bitmap = Bitmap.createBitmap(WIDTH, HEIGHT, Bitmap.Config.ARGB_8888);
            Canvas canvas = new Canvas(bitmap);
            Paint paint = new Paint();
            long deadline = SystemClock.elapsedRealtime() + (preview ? 4000 : 12000);
            boolean first = true;
            int frameIdx = 0;

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

                long nowMs = SystemClock.elapsedRealtime();
                String transition = updateStatesAndDetectTransition(get, previousStates, tailBuffer, nowMs);
                if (transition != null) {
                    lastTransition = transition;
                }
                if ("RUN".equals(previousStates.get("zygote"))) {
                    phaseTracker.onZygoteRunning(nowMs);
                }

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
                if (first && !curPsi[0].valid) {
                    tailBuffer.push(nowMs, "M", TailKind.MODULE_ERROR, "ERR PSI UNAVAIL", "MOD:PSI");
                }

                draw(canvas, paint, get, preview, geometry, stageRef.get(), previousStates,
                     cpuPct, ioPct, memPct, devInfo, rules, phaseTracker.snapshot(nowMs),
                     lastTransition, tailBuffer.visibleSnapshot(), nowMs, frameIdx);
                submit(control, bitmap, first, geometryChanged ? geometry : null);

                if (first) {
                    System.out.println("first_frame_submitted_ms=" + SystemClock.elapsedRealtime());
                    first = false;
                }
                lastGeometry = geometry;
                frameIdx++;
                Thread.sleep(250);
            }
            System.out.println("exit=time_limit");
        } finally {
            Process logcatProc = logcatProcRef.getAndSet(null);
            if (logcatProc != null) {
                logcatProc.destroyForcibly();
            }
            Process errProc = errProcRef.getAndSet(null);
            if (errProc != null) {
                errProc.destroyForcibly();
            }
            eventReader.interrupt();
            errReader.interrupt();
            dmesgReader.interrupt();
            try (SurfaceControl.Transaction transaction = new SurfaceControl.Transaction()) {
                transaction.setVisibility(control, false).setBuffer(control, null).apply();
            } finally {
                control.release();
                if (bitmap != null) bitmap.recycle();
                System.out.println("released_ms=" + SystemClock.elapsedRealtime());
            }
        }
    }

    private static String sanitizeTailText(RuleSet rules, String raw, int maxLen) {
        if (raw == null || raw.isEmpty()) return "";
        if (rules != null && rules.hasSensitiveMarker(raw)) return "";
        StringBuilder out = new StringBuilder(Math.min(raw.length(), maxLen));
        boolean prevSpace = true;
        for (int i = 0; i < raw.length(); i++) {
            char ch = Character.toUpperCase(raw.charAt(i));
            boolean keep = FONT_CHARS.indexOf(ch) >= 0;
            boolean mapSpace = ch == ' ' || ch == '\t' || ch == '(' || ch == ')'
                || ch == '[' || ch == ']' || ch == ',' || ch == ';'
                || ch == '\'' || ch == '"' || ch == '!' || ch == '|';
            if (keep) {
                out.append(ch);
                prevSpace = false;
            } else if (mapSpace) {
                if (!prevSpace) {
                    out.append(' ');
                    prevSpace = true;
                }
            }
            if (out.length() >= maxLen) break;
        }
        int end = out.length();
        while (end > 0 && out.charAt(end - 1) == ' ') end--;
        return out.substring(0, end);
    }

    private static boolean isStackTraceNoise(String msg) {
        if (msg == null) return true;
        String trimmed = msg.trim();
        if (trimmed.isEmpty()) return true;
        if (trimmed.startsWith("at ") || trimmed.startsWith("Caused by:")
                || trimmed.startsWith("... ") || trimmed.startsWith("Suppressed:")) {
            return true;
        }
        return trimmed.length() >= 2 && trimmed.charAt(0) == '#' && Character.isDigit(trimmed.charAt(1));
    }

    private static String formatTailRow(TailEntry e) {
        String ts;
        if (e.tsMs < 0L) {
            ts = " -.--";
        } else {
            double sec = e.tsMs / 1000.0d;
            if (sec < 100.0d) {
                ts = String.format(Locale.ROOT, "%5.2f", sec);
            } else if (sec < 1000.0d) {
                ts = String.format(Locale.ROOT, "%5.1f", sec);
            } else {
                ts = String.format(Locale.ROOT, "%5d", Math.round(sec));
            }
        }
        String line = ts + " " + e.source + " " + e.text;
        return line.length() > TAIL_LINE_CHARS ? line.substring(0, TAIL_LINE_CHARS) : line;
    }

    private static Thread startEventReader(
            final AtomicReference<StageInfo> stageRef,
            final PhaseTracker phaseTracker,
            final TailBuffer tailBuffer,
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
                                    long tsMs = SystemClock.elapsedRealtime();
                                    try {
                                        long parsed = Long.parseLong(m.group(2));
                                        if (parsed > 0L) tsMs = parsed;
                                    } catch (NumberFormatException ignored) {
                                    }
                                    phaseTracker.onFrameworkTag(m.group(1), tsMs, next.label);
                                    tailBuffer.push(tsMs, "P", TailKind.STAGE, next.label);
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

    private static Thread startErrReader(
            final RuleSet rules,
            final TailBuffer tailBuffer,
            final AtomicReference<Process> procRef) {
        Thread thread = new Thread(new Runnable() {
            @Override
            public void run() {
                Process proc = null;
                try {
                    proc = new ProcessBuilder(
                        "/system/bin/logcat", "-b", "main,system,crash", "-v", "monotonic", "*:E"
                    ).redirectErrorStream(true).start();
                    procRef.set(proc);
                    try (BufferedReader reader = new BufferedReader(
                            new InputStreamReader(proc.getInputStream(), StandardCharsets.UTF_8))) {
                        String line;
                        while (!Thread.currentThread().isInterrupted() && (line = reader.readLine()) != null) {
                            Matcher m = MONO_ERR_PATTERN.matcher(line);
                            if (m.matches()) {
                                String msg = m.group(4);
                                if (isStackTraceNoise(msg)) continue;
                                long tsMs = Math.round(Double.parseDouble(m.group(1)) * 1000.0d);
                                String tag = m.group(3).trim();
                                String trimmedMsg = msg.trim();
                                if (rules.matchesModuleKey(tag) || rules.matchesModuleKey(trimmedMsg)) {
                                    tailBuffer.push(tsMs, "M", TailKind.MODULE_ERROR,
                                        "ERR " + tag + ": " + trimmedMsg, "MOD:" + tag);
                                } else if (!rules.matchesIgnoreTag(tag)) {
                                    String src = "F".equals(m.group(2)) ? "F" : "E";
                                    tailBuffer.push(tsMs, src, TailKind.ERROR, tag + ": " + trimmedMsg, tag);
                                }
                            }
                        }
                    }
                } catch (Exception ignored) {
                } finally {
                    if (proc != null) proc.destroyForcibly();
                }
            }
        }, "bootscope-errors");
        thread.setDaemon(true);
        thread.start();
        return thread;
    }

    private static boolean isModuleErrorHint(RuleSet rules, String raw) {
        if (!rules.matchesModuleKey(raw)) return false;
        String lower = raw.toLowerCase(Locale.ROOT);
        String[] words = {
            "denied", "permission", "error", "fail", "abort",
            "segfault", "cannot", "not found", "unable", "sigabrt", "sigsegv"
        };
        for (String w : words) {
            if (lower.contains(w)) return true;
        }
        return false;
    }

    private static Thread startDmesgInitReader(
            final RuleSet rules,
            final PhaseTracker phaseTracker,
            final TailBuffer tailBuffer) {
        Thread thread = new Thread(new Runnable() {
            @Override
            public void run() {
                Process proc = null;
                try {
                    proc = new ProcessBuilder("dmesg").redirectErrorStream(true).start();
                    try (BufferedReader reader = new BufferedReader(
                            new InputStreamReader(proc.getInputStream(), StandardCharsets.UTF_8))) {
                        String line;
                        while (!Thread.currentThread().isInterrupted() && (line = reader.readLine()) != null) {
                            Matcher m = DMESG_ANY_PATTERN.matcher(line);
                            if (!m.matches()) continue;
                            long tsMs = Math.round(Double.parseDouble(m.group(1)) * 1000.0d);
                            String raw = m.group(2);
                            if (!raw.startsWith("init: ")) {
                                if (raw.contains("KPI: ")) {
                                    phaseTracker.onQcomKpiLine(raw);
                                }
                                String low = raw.toLowerCase(Locale.ROOT);
                                if (rules.matchesModuleKey(raw) && (low.contains("selinux") || low.contains("avc:"))) {
                                    tailBuffer.push(tsMs, "M", TailKind.MODULE_ERROR,
                                        "ERR SELINUX DENY BOOTSCOPE", "MOD:SELINUX");
                                } else if (isModuleErrorHint(rules, raw)) {
                                    tailBuffer.push(tsMs, "M", TailKind.MODULE_ERROR,
                                        "ERR " + raw, "MOD:KMSG");
                                }
                                continue;
                            }
                            String body = raw.substring(6);
                            if (body.startsWith("starting service '")) {
                                int q2 = body.indexOf('\'', 18);
                                if (q2 > 18) {
                                    String svc = body.substring(18, q2);
                                    phaseTracker.onServiceStart(svc, tsMs);
                                    tailBuffer.push(tsMs, "K", TailKind.INFO, "+" + svc);
                                }
                            } else if (body.startsWith("Service '")) {
                                int q2 = body.indexOf('\'', 9);
                                if (q2 > 9) {
                                    String svc = body.substring(9, q2);
                                    long durMs = 0L;
                                    int tookIdx = body.indexOf("took ");
                                    if (tookIdx > 0) {
                                        int tEnd = tookIdx + 5;
                                        while (tEnd < body.length()
                                                && (Character.isDigit(body.charAt(tEnd)) || body.charAt(tEnd) == '.')) {
                                            tEnd++;
                                        }
                                        if (tEnd > tookIdx + 5) {
                                            try {
                                                durMs = Math.round(Double.parseDouble(body.substring(tookIdx + 5, tEnd)) * 1000.0d);
                                            } catch (NumberFormatException ignored) {
                                            }
                                        }
                                    }
                                    phaseTracker.onServiceExit(svc, tsMs, durMs);
                                    boolean isSelf = rules.matchesModuleKey(svc);
                                    int statusIdx = body.indexOf("exited with status ");
                                    if (statusIdx > 0) {
                                        int end = statusIdx + 19;
                                        while (end < body.length() && Character.isDigit(body.charAt(end))) end++;
                                        if (end > statusIdx + 19) {
                                            int rc = Integer.parseInt(body.substring(statusIdx + 19, end));
                                            if (rc != 0) {
                                                if (!isSelf && rules.matchesIgnoreSvc(svc)) continue;
                                                tailBuffer.push(
                                                    tsMs,
                                                    isSelf ? "M" : "E",
                                                    isSelf ? TailKind.MODULE_ERROR : TailKind.ERROR,
                                                    (isSelf ? "ERR " : "") + svc + " RC=" + rc,
                                                    isSelf ? ("MOD:" + svc) : "");
                                            }
                                        }
                                    }
                                }
                            } else if (body.startsWith("Command '") && body.contains("and failed:")) {
                                int q2 = body.indexOf('\'', 9);
                                if (q2 > 9) {
                                    String fullCmd = body.substring(9, q2);
                                    boolean isSelf = rules.matchesModuleKey(body);
                                    if (!isSelf && rules.matchesIgnoreCmd(fullCmd)) continue;
                                    String cmd = fullCmd;
                                    int sp = cmd.indexOf(' ');
                                    if (sp > 0) cmd = cmd.substring(0, sp);
                                    tailBuffer.push(
                                        tsMs,
                                        isSelf ? "M" : "E",
                                        isSelf ? TailKind.MODULE_ERROR : TailKind.ERROR,
                                        (isSelf ? "ERR CMD " : "CMD FAIL ") + cmd,
                                        isSelf ? "MOD:CMD" : "");
                                }
                            } else if (body.startsWith("processing action (")) {
                                int end = body.indexOf(')', 19);
                                if (end > 19) {
                                    String act = body.substring(19, end);
                                    int amp = act.lastIndexOf("&&");
                                    if (amp >= 0) act = act.substring(amp + 2).trim();
                                    if (isMajorInitAction(act)) {
                                        phaseTracker.onInitAction(act);
                                        tailBuffer.push(tsMs, "K", TailKind.STAGE, "ACT " + act);
                                    }
                                }
                            }
                        }
                    }
                } catch (Exception ignored) {
                } finally {
                    if (proc != null) proc.destroyForcibly();
                }
            }
        }, "bootscope-dmesg");
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
            Method get, Map<String, String> previousStates,
            TailBuffer tailBuffer, long nowMs) throws Exception {
        String latest = null;
        double nowSec = nowMs / 1000d;
        boolean initial = previousStates.isEmpty();
        for (String name : WATCHED_SERVICES) {
            String next = shortState(state(get, name));
            String prev = previousStates.put(name, next);
            if (!initial && prev != null && !prev.equals(next)) {
                String shortName = shortServiceName(name);
                latest = String.format(
                    Locale.ROOT, "+%.2fS %s %s>%s",
                    nowSec, shortName, prev, next
                );
                tailBuffer.push(nowMs, "S", TailKind.STAGE, shortName + " " + prev + ">" + next);
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

    private static String phaseDurLabel(PhaseNode n, long nowMs) {
        if (n.startMs < 0L) return "--";
        long end = (n.state == NodeState.DONE && n.endMs >= n.startMs) ? n.endMs : nowMs;
        if (end < n.startMs) return "--";
        return String.format(Locale.ROOT, "%.2fS", (end - n.startMs) / 1000.0d);
    }

    private static String formatPhaseRow(PhaseNode n, long nowMs, RuleSet rules) {
        String badge = "-PND";
        if (n.state == NodeState.DONE) badge = "+OK ";
        else if (n.state == NodeState.RUNNING) badge = ">RUN";

        long durMs = -1L;
        if (n.state == NodeState.DONE && n.startMs >= 0L && n.endMs >= n.startMs) {
            durMs = n.endMs - n.startMs;
        } else if (n.state == NodeState.RUNNING && n.startMs >= 0L && nowMs >= n.startMs) {
            durMs = nowMs - n.startMs;
        }
        String durStr;
        if (durMs >= 0L) {
            double sec = durMs / 1000.0d;
            if (sec < 100.0d) durStr = String.format(Locale.ROOT, "%5.2fS", sec);
            else if (sec < 1000.0d) durStr = String.format(Locale.ROOT, "%5.1fS", sec);
            else durStr = String.format(Locale.ROOT, "%5.0fS", sec);
        } else if ("BL".equals(n.id)) {
            durStr = "  RAW ";
        } else {
            durStr = "      ";
        }
        String prefix = String.format(Locale.ROOT, "%s %s %-4s ", badge, durStr, n.id);
        String det = sanitizeTailText(rules, n.detail, 41);
        String row = prefix + det;
        return row.length() > TAIL_LINE_CHARS ? row.substring(0, TAIL_LINE_CHARS) : row;
    }

    private static void draw(
            Canvas canvas, Paint paint, Method get, boolean preview,
            DisplayGeometry geometry, StageInfo stage,
            Map<String, String> states, int cpuPct, int ioPct, int memPct,
            DeviceInfo devInfo, RuleSet rules, PhaseSnapshot snap,
            String lastTransition, List<TailEntry> visibleTail,
            long nowMs, int frameIdx) throws Exception {
        String uptime = String.format(Locale.ROOT, "%.2fS", nowMs / 1000d);
        String modeLabel = geometry.label();

        canvas.drawColor(0xff101b24);
        paint.setColor(0xff7be0ca);
        canvas.drawRect(0, 0, 8, HEIGHT, paint);

        BootFont.draw(canvas, paint, preview ? "BOOTSCOPE / PREVIEW" : "BOOTSCOPE / BOOT", 24, 20, 3);

        paint.setColor(0xff9ec3d6);
        int modeX = Math.max(420, WIDTH - 24 - modeLabel.length() * 18);
        BootFont.draw(canvas, paint, modeLabel, modeX, 20, 3);

        paint.setColor(0xffedf4f7);
        BootFont.draw(canvas, paint, devInfo.line1, 24, 50, 2);
        paint.setColor(0xff9ec3d6);
        BootFont.draw(canvas, paint, devInfo.line2, 24, 70, 2);

        paint.setColor(0xffedf4f7);
        String headerMeta = String.format(Locale.ROOT, "UPTIME %-8s  STAGE %d/%d", uptime, stage.step, STAGE_TOTAL);
        BootFont.draw(canvas, paint, headerMeta, 24, 92, 3);

        int barTop = 122;
        int barBottom = 136;
        int segW = 113;
        int segGap = 6;
        for (int i = 0; i < snap.phases.length; i++) {
            PhaseNode node = snap.phases[i];
            int x0 = 24 + i * (segW + segGap);
            int x1 = x0 + segW;
            paint.setColor(0xff1e3242);
            canvas.drawRect(x0, barTop, x1, barBottom, paint);
            if (node.state == NodeState.DONE) {
                paint.setColor(0xff7be0ca);
                canvas.drawRect(x0, barTop, x1, barBottom, paint);
            } else if (node.state == NodeState.RUNNING) {
                int pulseW = Math.min(segW, 38 + (frameIdx % 4) * 18);
                long rdur = node.startMs >= 0L ? (nowMs - node.startMs) : 0L;
                paint.setColor(rdur >= 6000L ? 0xffff8c7b : 0xffedf4f7);
                canvas.drawRect(x0, barTop, x0 + pulseW, barBottom, paint);
                paint.setColor(0xff7be0ca);
                canvas.drawRect(x0, barTop, x1, barTop + 2, paint);
                canvas.drawRect(x0, barBottom - 2, x1, barBottom, paint);
            }
            int lblX = x0 + (segW - node.id.length() * 12) / 2;
            paint.setColor(node.state == NodeState.DONE ? 0xff7be0ca
                : (node.state == NodeState.RUNNING ? 0xffedf4f7 : 0xff9ec3d6));
            BootFont.draw(canvas, paint, node.id, lblX, 142, 2);
        }

        paint.setColor(0xff1e3242);
        canvas.drawRect(24, 164, WIDTH - 24, 166, paint);
        paint.setColor(0xff7be0ca);
        BootFont.draw(canvas, paint, "CRITICAL PATH", 24, 174, 2);
        String stageHdr = stage.rawMs.isEmpty() ? stage.label : (stage.label + " " + stage.rawMs + "MS");
        if (stageHdr.length() > 30) stageHdr = stageHdr.substring(0, 30);
        paint.setColor(0xffedf4f7);
        BootFont.draw(canvas, paint, stageHdr, WIDTH - 24 - stageHdr.length() * 12, 174, 2);

        int expandedIdx = -1;
        for (int i = snap.phases.length - 1; i >= 0; i--) {
            if (snap.phases[i].state == NodeState.RUNNING) {
                expandedIdx = i;
                break;
            }
        }

        String bc = property(get, "sys.boot_completed");
        String animExit = property(get, "service.bootanim.exit");
        String bcStr = (bc == null || bc.isEmpty()) ? "0" : bc;
        String animStr = (animExit == null || animExit.isEmpty()) ? "0" : animExit;

        String expLine1 = "";
        String expLine2 = "";
        int expCol2 = 0xff9ec3d6;
        String psiCompact = (cpuPct >= 0 && ioPct >= 0 && memPct >= 0)
            ? String.format(Locale.ROOT, "PSI C:%2d%% I:%2d%% M:%2d%%", cpuPct, ioPct, memPct)
            : "";

        if (expandedIdx == 1) {
            String act = snap.lastInitAction.isEmpty() ? "EARLY-INIT" : snap.lastInitAction;
            if ("load_persist_props_action".equals(act)) act = "PERSIST_PROPS";
            else if ("firmware_mounts_complete".equals(act)) act = "FW_MOUNTS";
            String b1 = !snap.maxInitSvc.isEmpty()
                ? String.format(Locale.ROOT, "ACT:%s  SVC:%d  MAX:%s %.2fS",
                    act, snap.activeCnt, snap.maxInitSvc, snap.maxInitDurMs / 1000.0d)
                : String.format(Locale.ROOT, "ACT:%s  ACTIVE_SVC:%d", act, snap.activeCnt);
            expLine1 = sanitizeTailText(rules, b1, 57);
            if (!snap.waitSvc.isEmpty() && snap.waitDurMs >= 200L) {
                expLine2 = sanitizeTailText(
                    rules,
                    String.format(Locale.ROOT, "WAIT:%s %.1fS", snap.waitSvc, snap.waitDurMs / 1000.0d),
                    34
                );
                expCol2 = snap.waitDurMs >= 3000L ? 0xffff8c7b : 0xffedf4f7;
            } else {
                expLine2 = "WAIT:IDLE";
                expCol2 = 0xff9ec3d6;
            }
        } else if (expandedIdx == 2) {
            String apexSt = property(get, "apexd.status");
            if (apexSt == null || apexSt.isEmpty()) apexSt = states.get("apexd");
            expLine1 = sanitizeTailText(
                rules,
                "ZYG:" + states.get("zygote") + "  APEX:" + apexSt + "  ARTD:" + states.get("artd"),
                57
            );
            if (!snap.waitSvc.isEmpty() && snap.waitDurMs >= 200L) {
                expLine2 = sanitizeTailText(
                    rules,
                    String.format(Locale.ROOT, "WAIT:%s %.1fS", snap.waitSvc, snap.waitDurMs / 1000.0d),
                    34
                );
                expCol2 = snap.waitDurMs >= 3000L ? 0xffff8c7b : 0xffedf4f7;
            } else {
                expLine2 = "WAIT:BOOT_PROGRESS_START";
                expCol2 = 0xffedf4f7;
            }
        } else if (expandedIdx == 3) {
            expLine1 = sanitizeTailText(
                rules,
                "STEP:" + stage.label + "  INIT:" + phaseDurLabel(snap.phases[1], nowMs)
                    + "  ZYG:" + phaseDurLabel(snap.phases[2], nowMs),
                57
            );
            expLine2 = "PRELOADING FRAMEWORK CLASSES";
            expCol2 = 0xffedf4f7;
        } else if (expandedIdx == 4) {
            expLine1 = sanitizeTailText(
                rules,
                "STEP:" + stage.label + "  ARTD:" + states.get("artd")
                    + "  PRE:" + phaseDurLabel(snap.phases[3], nowMs),
                57
            );
            expLine2 = (ioPct >= 30) ? "PKG SCAN IO BOUND" : "PKG/DEX VERIFY SCAN";
            expCol2 = (ioPct >= 30 || cpuPct >= 65) ? 0xffff8c7b : 0xffedf4f7;
        } else if (expandedIdx == 5) {
            expLine1 = sanitizeTailText(
                rules,
                "STEP:" + stage.label + "  PMS:" + phaseDurLabel(snap.phases[4], nowMs)
                    + "  WPA:" + states.get("wpa_supplicant"),
                57
            );
            expLine2 = String.format(Locale.ROOT, "BC=%s ANIM_EXIT=%s", bcStr, animStr);
            expCol2 = 0xffedf4f7;
        } else if (expandedIdx == 0) {
            expLine1 = sanitizeTailText(rules, snap.phases[0].detail, 57);
            expLine2 = "WAITING EARLY INIT";
        }

        int treeY = 196;
        for (int i = 0; i < snap.phases.length; i++) {
            PhaseNode node = snap.phases[i];
            int col = 0xff9ec3d6;
            if (node.state == NodeState.DONE) {
                col = 0xff7be0ca;
            } else if (node.state == NodeState.RUNNING) {
                long rdur = node.startMs >= 0L ? (nowMs - node.startMs) : 0L;
                col = rdur >= 6000L ? 0xffff8c7b : 0xffedf4f7;
            }
            paint.setColor(col);
            BootFont.draw(canvas, paint, formatPhaseRow(node, nowMs, rules), 24, treeY, 2);
            treeY += 21;

            if (i == expandedIdx) {
                int sub1Y = treeY;
                int sub2Y = treeY + 21;
                paint.setColor(0xff7be0ca);
                canvas.drawRect(30, sub1Y - 4, 32, sub2Y + 8, paint);
                canvas.drawRect(32, sub1Y + 6, 40, sub1Y + 8, paint);
                canvas.drawRect(32, sub2Y + 6, 40, sub2Y + 8, paint);
                paint.setColor(0xffedf4f7);
                BootFont.draw(canvas, paint, expLine1, 46, sub1Y, 2);
                paint.setColor(expCol2);
                BootFont.draw(canvas, paint, expLine2, 46, sub2Y, 2);
                if (!psiCompact.isEmpty()) {
                    paint.setColor((ioPct >= 30 || cpuPct >= 65) ? 0xffff8c7b : 0xff7be0ca);
                    BootFont.draw(canvas, paint, psiCompact, WIDTH - 24 - psiCompact.length() * 12, sub2Y, 2);
                }
                treeY += 42;
            }
        }
        if (expandedIdx < 0) {
            String sum1 = "ALL PHASES COLLAPSED / COMPLETE";
            if (!snap.maxInitSvc.isEmpty()) {
                sum1 = sanitizeTailText(
                    rules,
                    String.format(Locale.ROOT, "ALL PHASES DONE  MAX-INIT:%s %.2fS",
                        snap.maxInitSvc, snap.maxInitDurMs / 1000.0d),
                    59
                );
            }
            paint.setColor(0xff7be0ca);
            BootFont.draw(canvas, paint, sum1, 24, treeY, 2);
            if (!psiCompact.isEmpty()) {
                paint.setColor((ioPct >= 30 || cpuPct >= 65) ? 0xffff8c7b : 0xff7be0ca);
                BootFont.draw(canvas, paint, psiCompact, 24, treeY + 21, 2);
            }
        }

        paint.setColor(0xffedf4f7);
        String svcRow = String.format(
            Locale.ROOT, "SF:%-4s ANIM:%-4s ZYG:%-4s APEX:%-4s ARTD:%-4s WPA:%-4s",
            states.get("surfaceflinger"), states.get("bootanim"), states.get("zygote"),
            states.get("apexd"), states.get("artd"), states.get("wpa_supplicant")
        );
        BootFont.draw(canvas, paint, svcRow, 24, 370, 2);

        String footer = "WAITING TRANSITIONS".equals(lastTransition)
            ? String.format(Locale.ROOT, "BC=%s ANIM_EXIT=%s", bcStr, animStr)
            : "LAST " + lastTransition;
        if (footer.length() > 40) footer = footer.substring(0, 40);
        paint.setColor(0xff9ec3d6);
        BootFont.draw(canvas, paint, footer, 24, 391, 2);

        String rbuf = String.format(Locale.ROOT, "RULES:%s/%d", rules.source, rules.count);
        BootFont.draw(canvas, paint, rbuf, WIDTH - 24 - rbuf.length() * 12, 391, 2);

        paint.setColor(0xff1e3242);
        canvas.drawRect(24, 414, WIDTH - 24, 416, paint);
        paint.setColor(0xff7be0ca);
        BootFont.draw(canvas, paint, "EVENTS", 24, 425, 2);
        String legend = "K=INIT P=PROG S=SVC E=ERR M=MOD";
        paint.setColor(0xff9ec3d6);
        BootFont.draw(canvas, paint, legend, WIDTH - 24 - legend.length() * 12, 425, 2);

        if (visibleTail.isEmpty()) {
            BootFont.draw(canvas, paint, "WAITING BOOT EVENTS...", 24, 447, 2);
        } else {
            for (int i = 0; i < visibleTail.size() && i < MAX_TAIL_ROWS; i++) {
                TailEntry item = visibleTail.get(i);
                int rowY = 447 + i * 22;
                if (item.kind == TailKind.STAGE) {
                    paint.setColor(0xff7be0ca);
                } else if (item.kind == TailKind.ERROR) {
                    paint.setColor(0xffff8c7b);
                } else if (item.kind == TailKind.MODULE_ERROR) {
                    paint.setColor(0xff7be0ca);
                    canvas.drawRect(14, rowY, 18, rowY + 14, paint);
                } else {
                    paint.setColor(0xff9ec3d6);
                }
                BootFont.draw(canvas, paint, formatTailRow(item), 24, rowY, 2);
            }
        }
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
