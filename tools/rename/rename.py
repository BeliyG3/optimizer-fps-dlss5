"""Rename SDK paths and tokens; dry-run first, then apply and verify."""
from __future__ import annotations
import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
EXTENSIONS={".h",".hpp",".cpp",".inl",".hlsl",".hlsli",".def",".cmake",".in",".ps1",".cmd",".bat",".sh",".py",".yml",".yaml",".md"}
NAMED_FILES={"CMakeLists.txt","CMakePresets.json"}
SKIP_DIRS={".git","out","external","dist","dist-release","third_party","graphify-out","Models"}
SKIP_PREFIXES=("docs/superpowers/","tools/rename/","tools/bench12/run_","tools/bench12/obj/","tools/bench12/shaders/")
SKIP_FILES={"CHANGELOG.md","docs/dev/CHANGELOG_HISTORY.md","docs/dev/CLEANUP_26_26.md","docs/dev/ENV_FLAGS_DISPOSITION.md"}
@dataclass(frozen=True)
class Rule:
    kind:str
    old:str
    new:str
@dataclass(frozen=True)
class Compiled:
    rule:Rule
    pattern:re.Pattern|None
    template:bool
def load_map(path):
    result=[]
    for n,line in enumerate(path.read_text(encoding="utf-8").splitlines(),1):
        if not line.strip() or line.lstrip().startswith("#"): continue
        parts=line.split("\t")
        if len(parts)!=3 or parts[0] not in ("path","word","text","regex") or not all(parts):
            raise SystemExit(f"{path}:{n}: expected kind<TAB>old<TAB>new")
        result.append(Rule(*parts))
    return result
def word_pattern(old):
    head=r"(?<![A-Za-z0-9_])" if re.match(r"[A-Za-z0-9_]",old[0]) else ""
    tail=r"(?![A-Za-z0-9_])" if re.match(r"[A-Za-z0-9_]",old[-1]) else ""
    return re.compile(head+re.escape(old)+tail)
def compile_rule(rule):
    pattern=word_pattern(rule.old) if rule.kind=="word" else re.compile(rule.old) if rule.kind=="regex" else None
    return Compiled(rule,pattern,rule.kind=="regex")
def apply_rule(text,compiled):
    r=compiled.rule
    if compiled.pattern is None: return text.replace(r.old,r.new),text.count(r.old)
    return compiled.pattern.subn(r.new if compiled.template else lambda m:r.new,text)
def is_candidate(path):
    p=path.as_posix()
    return not (set(path.parts[:-1])&SKIP_DIRS or p in SKIP_FILES or p.startswith(SKIP_PREFIXES)) and (path.name in NAMED_FILES or path.suffix.lower() in EXTENSIONS)
def candidate_files(root):
    for directory,names,files in os.walk(root):
        names[:]=sorted(n for n in names if n not in SKIP_DIRS)
        for name in sorted(files):
            p=Path(directory,name)
            if is_candidate(p.relative_to(root)): yield p
def move_paths(root,rules,apply):
    count=0
    for r in rules:
        if r.kind!="path": continue
        src,dst=root/r.old,root/r.new
        if not src.exists() and dst.exists(): continue
        if not src.exists() or dst.exists(): raise SystemExit(f"invalid move: {r.old} -> {r.new}")
        print(f"move {r.old} -> {r.new}"); count+=1
        if apply:
            dst.parent.mkdir(parents=True,exist_ok=True)
            subprocess.run(["git","mv",r.old,r.new],cwd=root,check=True)
    return count
def read_text(path):
    try: return path.read_bytes().decode("utf-8")
    except UnicodeDecodeError: return None
def rewrite_files(root,compiled,apply):
    changed=total=0
    for p in candidate_files(root):
        text=read_text(p)
        if text is None: continue
        before=text
        for r in compiled:
            text,n=apply_rule(text,r); total+=n
        if text!=before:
            changed+=1; print(p.relative_to(root))
            if apply: p.write_bytes(text.encode("utf-8"))
    return changed,total
def verify(root,rules,compiled):
    bad=[r.old for r in rules if r.kind=="path" and (root/r.old).exists()]
    for p in candidate_files(root):
        text=read_text(p)
        if text is None: continue
        for n,line in enumerate(text.splitlines(),1):
            for r in compiled:
                if (r.rule.old in line if r.pattern is None else r.pattern.search(line)):
                    bad.append(f"{p.relative_to(root)}:{n}: {r.rule.old}")
    print("\n".join(bad) if bad else "no old names left")
    return int(bool(bad))
def main(argv):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root",type=Path,default=Path(__file__).resolve().parents[2])
    p.add_argument("--map",type=Path,required=True)
    mode=p.add_mutually_exclusive_group(required=True)
    for name in ("dry-run","apply","verify"): mode.add_argument("--"+name,action="store_true")
    a=p.parse_args(argv); root=a.root.resolve()
    rules=load_map(a.map if a.map.is_absolute() else root/a.map)
    compiled=[compile_rule(r) for r in rules if r.kind!="path"]
    if a.verify: return verify(root,rules,compiled)
    moved=move_paths(root,rules,a.apply); changed,total=rewrite_files(root,compiled,a.apply)
    print(f"moves={moved} files={changed} replacements={total} apply={a.apply}")
    return 0
if __name__=="__main__": sys.exit(main(sys.argv[1:]))
