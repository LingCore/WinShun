using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

// Did a panel-sized block of the screen change between two shots?
// Tiles of 32x32 px where most pixels changed; the largest 4-connected group.
public static class Diff {
    public const int Tile = 32;
    static byte[] Bytes(Bitmap b, out int stride) {
        var d = b.LockBits(new Rectangle(0, 0, b.Width, b.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        stride = d.Stride;
        var buf = new byte[d.Stride * b.Height];
        Marshal.Copy(d.Scan0, buf, 0, buf.Length);
        b.UnlockBits(d);
        return buf;
    }
    // Returns "tiles=N box=WxH@X,Y" of the largest group of changed tiles.
    public static string Largest(Bitmap a, Bitmap b, int threshold, double fraction) {
        int sa, sb;
        byte[] pa = Bytes(a, out sa), pb = Bytes(b, out sb);
        int tw = a.Width / Tile, th = a.Height / Tile;
        var changed = new bool[tw, th];
        for (int ty = 0; ty < th; ty++)
            for (int tx = 0; tx < tw; tx++) {
                int n = 0, c = 0;
                for (int y = ty * Tile; y < ty * Tile + Tile; y += 2)
                    for (int x = tx * Tile; x < tx * Tile + Tile; x += 2) {
                        int ia = y * sa + x * 4, ib = y * sb + x * 4;
                        int d = Math.Abs(pa[ia] - pb[ib]) + Math.Abs(pa[ia + 1] - pb[ib + 1]) + Math.Abs(pa[ia + 2] - pb[ib + 2]);
                        n++; if (d > threshold) c++;
                    }
                changed[tx, ty] = c > n * fraction;
            }
        var seen = new bool[tw, th];
        int best = 0, bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
        for (int ty = 0; ty < th; ty++)
            for (int tx = 0; tx < tw; tx++) {
                if (!changed[tx, ty] || seen[tx, ty]) continue;
                int count = 0, x0 = tx, y0 = ty, x1 = tx, y1 = ty;
                var q = new Queue<int[]>(); q.Enqueue(new[] { tx, ty }); seen[tx, ty] = true;
                while (q.Count > 0) {
                    var p = q.Dequeue(); count++;
                    x0 = Math.Min(x0, p[0]); y0 = Math.Min(y0, p[1]); x1 = Math.Max(x1, p[0]); y1 = Math.Max(y1, p[1]);
                    foreach (var o in new[] { new[] { 1, 0 }, new[] { -1, 0 }, new[] { 0, 1 }, new[] { 0, -1 } }) {
                        int nx = p[0] + o[0], ny = p[1] + o[1];
                        if (nx < 0 || ny < 0 || nx >= tw || ny >= th || seen[nx, ny] || !changed[nx, ny]) continue;
                        seen[nx, ny] = true; q.Enqueue(new[] { nx, ny });
                    }
                }
                if (count > best) { best = count; bx0 = x0; by0 = y0; bx1 = x1; by1 = y1; }
            }
        return "tiles=" + best + " box=" + ((bx1 - bx0 + 1) * Tile) + "x" + ((by1 - by0 + 1) * Tile) + "@" + (bx0 * Tile) + "," + (by0 * Tile);
    }
    public static int TilesOf(string r) { return int.Parse(r.Substring(6, r.IndexOf(' ') - 6)); }
}
