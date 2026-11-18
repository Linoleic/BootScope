package bootscope;

import android.graphics.Canvas;
import android.graphics.Paint;
import java.util.Locale;

/** Small bitmap glyphs for drawing before Android's font service is available. */
final class BootFont {
    private static final String CHARACTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./:-_=+@>%?";
    // Each hex pair is one row of a five-pixel-wide glyph.
    private static final String[] GLYPHS = {
        "0E11111F111111", "1E11111E11111E", "0E11101010110E", "1E11111111111E",
        "1F10101E10101F", "1F10101E101010", "0E11101711110F", "1111111F111111",
        "1F04040404041F", "0702020212120C", "11121418141211", "1010101010101F",
        "111B1515111111", "11191513111111", "0E11111111110E", "1E11111E101010",
        "0E11111115120D", "1E11111E141211", "0F10100E01011E", "1F040404040404",
        "1111111111110E", "11111111110A04", "1111111515150A", "11110A040A1111",
        "11110A04040404", "1F01020408101F",
        "0E11131519110E", "040C040404040E", "0E11010204081F", "1E01010E01011E",
        "02060A121F0202", "1F10101E01011E", "0E10101E11110E", "1F010204080808",
        "0E11110E11110E", "0E11110F01010E",
        "00000000000C0C", "01020204080810", "000C0C000C0C00", "0000001F000000",
        "0000000000001F", "00001F001F0000", "0004041F040400", "0E11171517100E",
        "10080402040810", "18190204081303", "0E110102040004"
    };
    private static final int FALLBACK_INDEX = CHARACTERS.indexOf('?');

    private BootFont() {}

    static void draw(Canvas canvas, Paint paint, String text, int x, int y, int scale) {
        if (text == null) return;
        String upper = text.toUpperCase(Locale.ROOT);
        for (int i = 0; i < upper.length(); i++, x += scale * 6) {
            char ch = upper.charAt(i);
            if (ch == ' ') continue;
            int index = CHARACTERS.indexOf(ch);
            String glyph = GLYPHS[index < 0 ? FALLBACK_INDEX : index];
            for (int row = 0; row < 7; row++) {
                int bits = Character.digit(glyph.charAt(row * 2), 16) * 16
                        + Character.digit(glyph.charAt(row * 2 + 1), 16);
                for (int col = 0; col < 5; col++) {
                    if ((bits & (16 >> col)) == 0) continue;
                    int left = x + col * scale;
                    int top = y + row * scale;
                    canvas.drawRect(left, top, left + scale, top + scale, paint);
                }
            }
        }
    }
}
