"""FFmpeg n8.1 scalar MinGW test bootstrap for sandboxes where MSYS cannot run.
This is deliberately NOT a portable substitute for FFmpeg configure.
Only generated configuration/registries are written, under ignored work/.
The upstream C sources are not modified. Use build.sh for production/Android.
"""
import pathlib,re,subprocess,concurrent.futures,sys,os
root=pathlib.Path(__file__).resolve().parents[2]/'work/bink_native'
src=root/'ffmpeg-src';out=root/'ffmpeg-native';out.mkdir(exist_ok=True)
pin='9047fa1b084f76b1b4d065af2d743df1b40dfb56'
actual=subprocess.check_output(['git','-C',str(src),'rev-parse','HEAD'],text=True).strip()
if actual!=pin:sys.exit('Expected official FFmpeg n8.1 commit '+pin+', got '+actual)
mingw=pathlib.Path(os.environ.get('MINGW_BIN','C:/msys64/mingw64/bin'))
libs=['avutil','avcodec','avformat','swscale']
macros=set()
for lib in libs:
 for f in (src/('lib'+lib)).rglob('*'):
  if f.suffix in ('.c','.h'):macros.update(re.findall(r'\b(?:HAVE|CONFIG|ARCH)_[A-Z0-9_]+\b',f.read_text(errors='ignore')))
macros.discard('HAVE_AV_CONFIG_H');macros.add('HAVE_LSX')
enabled='CONFIG_BINK_DEMUXER CONFIG_BINK_DECODER CONFIG_BINKAUDIO_DCT_DECODER CONFIG_BINKAUDIO_RDFT_DECODER CONFIG_FILE_PROTOCOL CONFIG_AVUTIL CONFIG_AVCODEC CONFIG_AVFORMAT CONFIG_SWSCALE CONFIG_BLOCKDSP CONFIG_HPELDSP CONFIG_WMA_FREQS CONFIG_SAFE_BITSTREAM_READER CONFIG_SWSCALE_ALPHA HAVE_FAST_UNALIGNED HAVE_ATOMICS_NATIVE HAVE_ALIGNED_MALLOC HAVE_ALIGNED_STACK HAVE_DOS_PATHS HAVE_WINDOWS_H HAVE_IO_H HAVE_MALLOC_H HAVE_UNISTD_H HAVE_STDINT_H HAVE_SYS_PARAM_H HAVE_SYS_TIME_H HAVE_GETTIMEOFDAY HAVE_GETPROCESSTIMES HAVE_SLEEP HAVE_MAPVIEWOFFILE HAVE_VIRTUALALLOC HAVE_SETCONSOLETEXTATTRIBUTE HAVE_KBHIT HAVE_ATTRIBUTE_PACKED HAVE_PRAGMA_DEPRECATED HAVE_LOCAL_ALIGNED HAVE_LOCAL_ALIGNED_8 HAVE_LOCAL_ALIGNED_16 HAVE_LOCAL_ALIGNED_32 HAVE_LOCAL_ALIGNED_64 HAVE_ISATTY HAVE_CBRT HAVE_CBRTF HAVE_COPYSIGN HAVE_COPYSIGNF HAVE_ERF HAVE_EXP2 HAVE_EXP2F HAVE_HYPOT HAVE_ISFINITE HAVE_ISINF HAVE_ISNAN HAVE_LDEXPF HAVE_LLRINT HAVE_LLRINTF HAVE_LOG2 HAVE_LOG2F HAVE_LRINT HAVE_LRINTF HAVE_RINT HAVE_ROUND HAVE_ROUNDF HAVE_TRUNC HAVE_TRUNCF HAVE_POWF HAVE_SINF HAVE_COSF'.split()
# No assembly/architecture code and no optional host hardware/network/threads.
macros.update(enabled)
lines=['#ifndef FFMPEG_CONFIG_H','#define FFMPEG_CONFIG_H']
lines += ['#define '+m+' '+str(int(m in enabled)) for m in sorted(macros)]
lines += ['#define EXTERN_PREFIX ""','#define EXTERN_ASM',' #define av_restrict restrict','#define SWS_MAX_FILTER_SIZE 256','#define FFMPEG_CONFIGURATION "native scalar test bootstrap"','#define FFMPEG_LICENSE "LGPL version 2.1 or later"','#define CONFIG_THIS_YEAR 2026','#define AVCONV_DATADIR ""','#define CC_IDENT "MinGW GCC"','#define OS_NAME "mingw32"','#define BUILDSUF ""','#define SLIBSUF ".dll"','#define SLIBNAME ""','#define SLIBNAME_WITH_MAJOR ""','#define SLIBNAME_WITH_VERSION ""','#endif']
config='\n'.join(lines)+'\n'
if not (out/'config.h').exists() or (out/'config.h').read_text()!=config:
 (out/'config.h').write_text(config)
(out/'config_components.h').write_text('#include "config.h"\n')
(out/'libavutil').mkdir(exist_ok=True)
(out/'libavutil/avconfig.h').write_text('#define AV_HAVE_BIGENDIAN 0\n#define AV_HAVE_FAST_UNALIGNED 1\n')
(out/'libavutil/ffversion.h').write_text('#define FFMPEG_VERSION "8.1-native-probe"\n')
lists={'libavcodec/codec_list.c':'static const FFCodec * const codec_list[] = { &ff_bink_decoder, &ff_binkaudio_dct_decoder, &ff_binkaudio_rdft_decoder, NULL };','libavcodec/bsf_list.c':'static const FFBitStreamFilter * const bitstream_filters[] = { NULL };','libavcodec/parser_list.c':'static const AVCodecParser * const parser_list[] = { NULL };','libavformat/demuxer_list.c':'static const FFInputFormat * const demuxer_list[] = { &ff_bink_demuxer, NULL };','libavformat/muxer_list.c':'static const FFOutputFormat * const muxer_list[] = { NULL };','libavformat/protocol_list.c':'static const URLProtocol * const url_protocols[] = { &ff_file_protocol, NULL };'}
for file,txt in lists.items():
 dest=out/file;dest.parent.mkdir(exist_ok=True);dest.write_text(txt+'\n')
commands=[];objects={}
for lib in libs:
 text=(src/('lib'+lib)/'Makefile').read_text().replace('\\\n',' ')
 selected=[]
 for line in text.splitlines():
  m=re.match(r'OBJS\s*(?:\+?=)(.*)',line)
  if m:selected += re.findall(r'[\w/]+\.o',m[1])
  m=re.match(r'(?:OBJS|STLIBOBJS)-\$\((CONFIG_\w+|HAVE_\w+)\)\s*\+=(.*)',line)
  if m and m[1] in enabled:selected += re.findall(r'[\w/]+\.o',m[2])
 if lib=='avutil':selected += ['hwcontext_stub.o','half2float.o']
 if lib=='avcodec':selected += ['float_scalarproduct.o','timecode_internal.o']
 if lib=='swscale':selected += ['half2float.o']
 objects[lib]=[]
 for obj in sorted(set(selected)):
  source=src/('lib'+lib)/obj.replace('.o','.c')
  if not source.exists():
   source=src/'libavutil'/obj.replace('.o','.c')
  target=out/('lib'+lib)/obj;target.parent.mkdir(parents=True,exist_ok=True);objects[lib].append(str(target))
  commands.append((target,[str(mingw/'gcc.exe'),'-c','-O2','-std=c17','-DHAVE_AV_CONFIG_H','-D_ISOC99_SOURCE','-D_FILE_OFFSET_BITS=64','-D_WIN32_WINNT=0x0602','-DWIN32_LEAN_AND_MEAN','-D__USE_MINGW_ANSI_STDIO=1','-Wno-deprecated-declarations','-I'+str(out),'-I'+str(src),str(source),'-o',str(target)]))
def compile(item):
 target,cmd=item
 if target.exists() and target.stat().st_mtime>(out/'config.h').stat().st_mtime:return True
 p=subprocess.run(cmd,capture_output=True,text=True)
 if p.returncode:
  (target.with_suffix('.err')).write_text(p.stdout+p.stderr);return False
 return True
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
 results=list(ex.map(compile,commands))
print('compiled',sum(results),'/',len(results))
if not all(results):
 for (t,c),ok in zip(commands,results):
  if not ok:print(t.name, t.with_suffix('.err').read_text()[:1600])
 sys.exit(1)
for lib in libs:
 subprocess.run([str(mingw/'ar.exe'),'rcs',str(out/('lib'+lib+'.a'))]+objects[lib],check=True)
print('native archives built')
