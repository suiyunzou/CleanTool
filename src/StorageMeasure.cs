using System;
using System.IO;
using System.Collections.Generic;
using System.Diagnostics;

// Read metadata only. A partial traversal is always reported as a lower bound.
public sealed class StorageMeasurement {
    public long Bytes;
    public int Files;
    public int Skipped;
    public bool Complete = true;
}
public static class StorageMeasure {
    public static StorageMeasurement Read(string path, int milliseconds) {
        var r = new StorageMeasurement();
        var clock = Stopwatch.StartNew();
        var pending = new Stack<string>();
        pending.Push(path);
        while (pending.Count > 0) {
            if (clock.ElapsedMilliseconds >= milliseconds || r.Files >= 200000) { r.Complete = false; break; }
            var dir = pending.Pop();
            try {
                var info = new DirectoryInfo(dir);
                for (var parent = info; parent != null; parent = parent.Parent)
                    if ((parent.Attributes & FileAttributes.ReparsePoint) != 0) throw new IOException("Link skipped");
                foreach (var item in info.EnumerateFileSystemInfos()) {
                    if (clock.ElapsedMilliseconds >= milliseconds || r.Files >= 200000) { r.Complete = false; break; }
                    if ((item.Attributes & FileAttributes.ReparsePoint) != 0) { r.Skipped++; r.Complete = false; continue; }
                    if ((item.Attributes & FileAttributes.Directory) != 0) pending.Push(item.FullName);
                    else { r.Bytes += ((FileInfo)item).Length; r.Files++; }
                }
            } catch { r.Skipped++; r.Complete = false; }
        }
        return r;
    }
}
