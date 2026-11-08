using System;
using System.IO;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Runtime.InteropServices;
using System.Threading;

public class CacheFile {
    public string Name;
    public long Length;
    public string Hash;
    public DateTime Created;
    public DateTime Modified;
}
// No recursive deletion and no traversal through junctions or symbolic links.
public static class CacheTransfer {
    [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr SendMessageTimeout(IntPtr h,uint msg,IntPtr w,string l,uint flags,uint timeout,out IntPtr result);
    public static void NotifyEnvironment() {
        ThreadPool.QueueUserWorkItem(delegate { IntPtr result; SendMessageTimeout(new IntPtr(0xffff),0x1a,IntPtr.Zero,"Environment",2,100,out result); });
    }
    public static void Plain(string path) {
        var p = Path.GetFullPath(path);
        while (!String.IsNullOrEmpty(p)) {
            if ((File.Exists(p) || Directory.Exists(p)) && (File.GetAttributes(p) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("目录包含链接，未迁移：" + p);
            p = Path.GetDirectoryName(p);
        }
    }
    public static string Child(string root, string name) {
        var prefix = Path.GetFullPath(root).TrimEnd('\\') + "\\";
        var p = Path.GetFullPath(Path.Combine(prefix, name));
        if (!p.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) throw new IOException("文件超出缓存目录");
        Plain(p); return p;
    }
    static string Hash(Stream s) {
        s.Position = 0;
        using (var sha = SHA256.Create()) { return BitConverter.ToString(sha.ComputeHash(s)).Replace("-", ""); }
    }
    public static CacheFile[] Read(string root) { return Read(root,null,null); }
    public static CacheFile[] Read(string root,Action<int,long> progress,Func<bool> cancel) {
        Plain(root);
        var prefix = Path.GetFullPath(root).TrimEnd('\\') + "\\";
        var result = new List<CacheFile>();
        var pending = new Stack<string>(); pending.Push(root);
        long bytes=0;
        while (pending.Count > 0) {
            if(cancel!=null && cancel()) throw new OperationCanceledException("已取消");
            var dir = pending.Pop(); Plain(dir);
            foreach(var d in Directory.GetDirectories(dir)) { Plain(d); pending.Push(d); }
            foreach(var p in Directory.GetFiles(dir)) {
                if(cancel!=null && cancel()) throw new OperationCanceledException("已取消");
                Plain(p); var f = new FileInfo(p);
                using(var s = new FileStream(p, FileMode.Open, FileAccess.Read, FileShare.Read)) {
                    result.Add(new CacheFile { Name=p.Substring(prefix.Length), Length=s.Length, Hash=Hash(s), Created=f.CreationTimeUtc, Modified=f.LastWriteTimeUtc });
                    bytes+=s.Length;
                }
                if(progress!=null && result.Count%50==0) progress(result.Count,bytes);
            }
        }
        result.Sort((a,b) => StringComparer.OrdinalIgnoreCase.Compare(a.Name,b.Name));
        return result.ToArray();
    }
    public static void Match(string root, CacheFile[] expected) {
        var actual = Read(root);
        if (actual.Length != expected.Length) throw new IOException("缓存文件数量发生变化");
        for(int i=0;i<actual.Length;i++) {
            if(!String.Equals(actual[i].Name, expected[i].Name, StringComparison.OrdinalIgnoreCase) || actual[i].Length != expected[i].Length || actual[i].Hash != expected[i].Hash)
                throw new IOException("缓存内容发生变化：" + actual[i].Name);
        }
    }
    public static void CopyFile(string source, string dest, CacheFile f) {
        Plain(source); Plain(dest);
        using(var s = new FileStream(source,FileMode.Open,FileAccess.Read,FileShare.Read)) {
            if(s.Length != f.Length || Hash(s) != f.Hash) throw new IOException("文件已变化："+f.Name);
            Directory.CreateDirectory(Path.GetDirectoryName(dest));
            using(var d = new FileStream(dest,FileMode.CreateNew,FileAccess.ReadWrite,FileShare.None)) {
                s.Position=0; s.CopyTo(d); d.Flush(true);
                if(d.Length != f.Length || Hash(d) != f.Hash) throw new IOException("复制校验失败："+f.Name);
            }
        }
        File.SetCreationTimeUtc(dest,f.Created); File.SetLastWriteTimeUtc(dest,f.Modified);
    }
    public static void Copy(string source, string dest, CacheFile[] files) {
        Copy(source,dest,files,null,null);
    }
    public static void Copy(string source, string dest, CacheFile[] files,Action<int,long> progress,Func<bool> cancel) {
        int count=0;long bytes=0;
        foreach(var f in files) {
            if(cancel!=null && cancel()) throw new OperationCanceledException("已取消");
            CopyFile(Child(source,f.Name),Child(dest,f.Name),f);
            count++;bytes+=f.Length;if(progress!=null && (count%20==0 || count==files.Length)) progress(count,bytes);
        }
        // Every copied stream is verified above; caller verifies the complete tree after configuration.
    }
    public static void RemoveOwnedSubset(string root,CacheFile[] expected) {
        var actual=Read(root);var map=new Dictionary<string,CacheFile>(StringComparer.OrdinalIgnoreCase);
        foreach(var f in expected) map.Add(f.Name,f);
        foreach(var f in actual) {
            CacheFile e;if(!map.TryGetValue(f.Name,out e) || f.Hash!=e.Hash || f.Length!=e.Length) throw new IOException("目标有新增或变化内容，已保留");
        }
        Remove(root,actual);
    }
    // Only remove manifest files after checking their contents. Extra/new files stop removal.
    public static long Remove(string root, CacheFile[] files) {
        Match(root,files); long bytes=0;
        foreach(var f in files) {
            var p=Child(root,f.Name);
            using(var s=new FileStream(p,FileMode.Open,FileAccess.Read,FileShare.None)) {
                if(s.Length!=f.Length || Hash(s)!=f.Hash) throw new IOException("删除前文件已变化："+f.Name);
            }
            File.Delete(p);
            if(File.Exists(p)) throw new IOException("未能删除旧缓存："+f.Name);
            bytes+=f.Length;
        }
        EmptyDirectories(root); return bytes;
    }
    public static void EmptyDirectories(string root) {
        Plain(root);
        foreach(var d in Directory.GetDirectories(root)) EmptyDirectories(d);
        Directory.Delete(root,false);
    }
    // Restore missing files from the verified copy. Never overwrite changed files.
    public static void Restore(string backup, string verified, CacheFile[] files) {
        Restore(backup,verified,files,null);
    }
    public static void Restore(string backup, string verified, CacheFile[] files,Action<int,long> progress) {
        Plain(backup); Directory.CreateDirectory(backup);
        int count=0;long bytes=0;
        foreach(var f in files) {
            var p=Child(backup,f.Name);
            if(File.Exists(p)) {
                using(var s=new FileStream(p,FileMode.Open,FileAccess.Read,FileShare.Read)) {
                    if(s.Length!=f.Length || Hash(s)!=f.Hash) throw new IOException("恢复位置有不同内容，已保留："+f.Name);
                }
            } else CopyFile(Child(verified,f.Name),p,f);
            count++;bytes+=f.Length;if(progress!=null && (count%20==0 || count==files.Length)) progress(count,bytes);
        }
        // Caller verifies the complete restored tree at its original path after the rename.
    }
}
