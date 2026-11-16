"""Survey of the post-GBuffer shaders (deferred lighting, composite, tiled CS, sky/water/particles/effects
and all image-space groups) for stereo-lighting-post.md.

usage: python -I stereo_post_survey.py <flat111_dir> <flat110_dir> <vr_dir> [group ...]

Group names: image-space groups are named from the engine's construction order. Every BSImagespaceShader
calls BSShader::Load on the shared fxp stream from BSImagespaceShader::Initialize, so fxp group 18+i is the
i-th shader constructed in ImageSpaceManager::InitEffects (1.10 1427DB4D0, call order read in Ghidra;
1.11 2186DA0 has the same order plus ISCopyNormals as the 26th). Groups 0..11 are BSShader::Load order in
BSShaderManager::Initialize (1.10 1427D4F..); 12.. are the compute InitSDMs.
VR groups are paired to flat110 by align.align_dirs and each pair is scored by opcode similarity.

Per shader it prints: cb12 rows read (static and dynamic base), cb2 size, SV_Position use, textures,
sample/load counts, and for the VR counterpart: identical or the stereo features it gained and the
cb12/cb8/eye-varying use.
"""
import difflib, json, os, re, sys
from collections import Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import align as al

MAT = {0: 'BloodSplatter', 1: 'DistantTree', 2: 'Particle', 3: 'Sky', 4: 'Effect', 5: 'Lighting(forward)',
       6: 'Utility(depth/shadow)', 7: 'Water', 8: 'DFPrePass(GBuffer)', 9: 'DFLight(deferred lights)',
       10: 'DFComposite', 11: 'FaceCustomization', 12: 'CS:BSDFTiledLighting', 13: 'CS:(DFDecals?)',
       14: 'CS:(MeshCombiner?)', 15: 'CS:(g015?)', 16: 'CS:(g016?)', 17: 'CS:(g017?)'}

IS110 = """GammaCorrectLUT FXAA Copy CopyScaleBias CopyVisAlpha Refraction DoubleVision TextureMask DepthOfField
BokehDepthOfFieldPass1 BokehDepthOfFieldPass2 BokehDepthOfFieldPass3 BokehDepthOfFieldPass4 BokehDepthOfFieldPass4Fogged
DistantBlur FullScreenColor Map RadialBlur RadialBlurMedium RadialBlurHigh GreyScale DownsampleDepth CopyStencil
CopyWaterMask CopyShadowMapToArray HDRTonemapBlendCinematic HDRTonemapBlendCinematicFade HDRDownSample16 HDRDownSample4
HDRDownSample16Lum HDRDownSample4RGB2Lum HDRDownSample4LightAdapt HDRDownSample4LumClamp HDRDownSample16LightAdapt
HDRDownSample16LumClamp HDRDownSample4CS HDRDownSample64RGB2LumCS HDRDownSample4LumCS HDRDownSample16LumCS
HDRDownSample2LightAdaptCS Blur3 Blur5 Blur7 Blur9 Blur11 Blur13 Blur15 HDRBlurX15_320CS HDRBlurX15_480CS
HDRBlurX15_1024CS NonHDRBlur3 NonHDRBlur5 NonHDRBlur7 NonHDRBlur9 NonHDRBlur11 NonHDRBlur13 NonHDRBlur15
BrightPassBlur3 BrightPassBlur5 BrightPassBlur7 BrightPassBlur9 BrightPassBlur11 BrightPassBlur13 BrightPassBlur15
BrightPassHDRBlurY15_180CS BrightPassHDRBlurY15_270CS BrightPassHDRBlurY15_1024CS WaterDisplacementClearSimulation
WaterDisplacementTexOffset WaterDisplacementWadingRipple WaterDisplacementRainRipple WaterWadingHeightmap
WaterRainHeightmap WaterBlendHeightmaps WaterSmoothHeightmap WaterDisplacementNormals NoiseScrollAndBlend
NoiseNormalmap LocalMap LocalMapCompanion AlphaBlend PipboyScreen HUDGlass HUDGlassDropShadow HUDGlassBlurY
HUDGlassBlurX HUDGlassMarkers HUDGlassClear HUDGlassCopy VatsTargetDebug VatsTarget ModMenuEffect
ModMenuGlowComposite AmbientOcclusion AmbientOcclusionBlur VLSSpotLight VLSApplication VLSComposite VLSSliceCoord
VLSSliceInterp VLSSliceStencil VLSSliceScatterRay VLSSliceScatterInterp VLSScatterAccum SAOCameraZ SAOMinify
SAORawAO SAOBlurH SAOBlurV SAORawAOEditor SAOCameraZCS SAOMinifyCS SAOCameraAndMipsZCS SAOMipsZCS SAORawAOCS
SAOBlurHCS SAOBlurVCS MotionBlur TemporalAA TemporalAAPipboy TemporalAAPowerArmorPipboy GammaCorrect
GammaLinearize GammaCorrectResize Sunbeams SSLRRaytracing SSLRBlurH SSLRBlurV SSLRPrepass DepthOfFieldFogged
DepthOfFieldSplitScreen DistantBlurFogged LensFlare RainSplash RainSplashUpdate RainSplashDraw LensFlareVisibility
UpsampleDynamicResolution BlurY3_270CS BlurY5_270CS BlurY7_270CS BlurY9_270CS BlurY11_270CS BlurY13_270CS
BlurY15_270CS BlurX3_480CS BlurX5_480CS BlurX7_480CS BlurX9_480CS BlurX11_480CS BlurX13_480CS BlurX15_480CS
BrightPassBlurY3_270CS BrightPassBlurY5_270CS BrightPassBlurY7_270CS BrightPassBlurY9_270CS BrightPassBlurY11_270CS
BrightPassBlurY13_270CS BrightPassBlurY15_270CS""".split()
assert len(IS110) == 159, len(IS110)
IS111 = IS110[:25] + ['CopyNormals'] + IS110[25:]


def names110():
    d = dict(MAT)
    for i, n in enumerate(IS110):
        d[18 + i] = 'IS:' + n
    return d


def names111():
    d = dict(MAT)
    for i, n in enumerate(IS111):
        d[18 + i] = 'IS:' + n
    return d


def text(d, s):
    return open(os.path.join(d, 'disasm', s['name'] + '.txt')).read()


def body(t):
    return t.split('dcl_temps')[-1] if 'dcl_temps' in t else t


def facts(s, t):
    b = body(t)
    f = {}
    rows = sorted({int(x) for x in re.findall(r'cb12\[(\d+)\]', b)})
    dyn = sorted({int(x) for x in re.findall(r'cb12\[r\d+\.[xyzw] \+ (\d+)\]', b)})
    f['cb12'] = rows
    f['cb12dyn'] = dyn
    m = re.search(r'dcl_constantbuffer CB2\[(\d+)\]', t)
    f['cb2'] = int(m.group(1)) if m else 0
    f['cb8'] = 'CB8[' in t or 'cb8[' in b
    f['svpos'] = bool(re.search(r'dcl_input_ps_siv linear noperspective v0\.\w+, position', t)) and bool(
        re.search(r'[ ,(-]v0\.[xy]', b))
    f['tex'] = len(re.findall(r'dcl_resource_texture', t)) + len(re.findall(r'dcl_resource_structured', t))
    f['uav'] = len(re.findall(r'dcl_uav', t))
    f['samp'] = len(re.findall(r'\bsample\w*', b))
    f['ld'] = len(re.findall(r'\bld\w*\(', b)) + len(re.findall(r'\bld_\w+', b))
    f['eyein'] = bool(re.search(r'dcl_input_ps constant v\d+\.x', t))
    f['icb'] = 'dcl_immediateConstantBuffer' in t
    f['iid'] = bool(re.search(r'instance_id', t))
    f['clip'] = 'clip_distance' in t
    f['tid'] = bool(re.search(r'vThreadID|vThreadGroupID|vThreadIDInGroup', t))
    f['ninst'] = len([l for l in b.splitlines() if l.strip() and not l.startswith('//')])
    return f


def ops(t):
    return [l.split()[0] for l in body(t).splitlines() if l.strip() and not l.startswith('//') and l[0] not in '{}']


def short(f):
    s = []
    if f['cb12'] or f['cb12dyn']:
        s.append('cb12%s%s' % (f['cb12'], (' dyn+' + str(f['cb12dyn'])) if f['cb12dyn'] else ''))
    if f['cb2']:
        s.append('cb2[%d]' % f['cb2'])
    if f['cb8']:
        s.append('cb8')
    if f['svpos']:
        s.append('SV_Pos.xy')
    if f['eyein']:
        s.append('eye-varying')
    if f['icb']:
        s.append('icb')
    if f['iid']:
        s.append('SV_InstanceID')
    if f['clip']:
        s.append('clipdist')
    s.append('t%d s%d ld%d%s n%d' % (f['tex'], f['samp'], f['ld'], (' u%d' % f['uav']) if f['uav'] else '', f['ninst']))
    return ' '.join(s)


def main():
    d111, d110, dvr = sys.argv[1:4]
    want = {int(x) for x in sys.argv[4:]}
    i111, g111 = al.load(d111)
    i110, g110 = al.load(d110)
    ivr, gvr = al.load(dvr)
    n110, n111 = names110(), names111()
    inv111 = {v: k for k, v in n111.items()}
    pairs, una, unb = al.align_dirs(d110, dvr)
    p110vr = dict(pairs)
    print('# align.py: flat110 unpaired', una, 'vr unpaired', unb)
    # align.py pairs image-space groups positionally, which slips where VR inserted or removed classes
    # (VR adds FullScreenVR + HUDGlassMaskRects, drops the 3 RainSplash shaders). Re-pair g>=12 by content:
    # best VR group in a +-4 window, score = mean over shared (stage,id) of 1 (same sha1) or opcode similarity.
    cache = {}

    def gscore(a, b):
        ea = {(s['stage'], s['id']): s for s in g110.get(a, [])}
        eb = {(s['stage'], s['id']): s for s in gvr.get(b, [])}
        ks = ea.keys() & eb.keys()
        if not ks or len(ea) != len(eb):
            return 0.0
        tot = 0.0
        for k in ks:
            if ea[k]['sha1'] == eb[k]['sha1']:
                tot += 1.0
                continue
            ka = ('a', ea[k]['name'])
            kb = ('b', eb[k]['name'])
            if ka not in cache:
                cache[ka] = ops(text(d110, ea[k]))
            if kb not in cache:
                cache[kb] = ops(text(dvr, eb[k]))
            tot += difflib.SequenceMatcher(None, cache[ka], cache[kb], autojunk=False).ratio()
        return tot / len(ks)

    off = 0
    positional = set()
    for a in sorted(g110):
        if a < 12:
            continue
        sc = [(gscore(a, b), b) for b in range(a - 4, a + 5) if b in gvr]
        top = max(sc, default=(0, None))[0]
        cand = [b for s, b in sc if s >= top - 0.02 and s >= 0.6]
        if cand:
            b = a + off if (a + off) in cand else min(cand, key=lambda b: abs(b - a - off))
            p110vr[a] = b
            off = b - a
        elif (a + off) in gvr and len(gvr[a + off]) == len(g110[a]):
            p110vr[a] = a + off      # positional fallback: same shape, heavily rewritten by VR
            positional.add(a)
        else:
            p110vr.pop(a, None)
    used = Counter(p110vr.values())
    print('# positional fallback pairs (flat110 groups):', sorted(positional))
    print('# content pairing: VR groups claimed twice', [g for g, n in used.items() if n > 1],
          '; VR groups unclaimed', sorted(set(gvr) - set(p110vr.values())),
          '; flat110 groups without VR match', sorted(set(g110) - set(p110vr)))
    summary = Counter()
    for g in sorted(g111):
        if want and g not in want:
            continue
        name = n111.get(g, '?')
        g10 = {v: k for k, v in n110.items()}.get(name)
        gv = p110vr.get(g10) if g10 is not None else None
        print('\n== g%03d (1.11) %s   flat110 g%s  vr g%s' % (g, name, '%03d' % g10 if g10 is not None else '--',
                                                            '%03d' % gv if gv is not None else '--'))
        ea10 = {(s['stage'], s['id']): s for s in g110.get(g10, [])} if g10 is not None else {}
        evr = {(s['stage'], s['id']): s for s in gvr.get(gv, [])} if gv is not None else {}
        for s in sorted(g111[g], key=lambda s: (s['stage'], s['id'])):
            k = (s['stage'], s['id'])
            t = text(d111, s)
            f = facts(s, t)
            line = '  %s %08x  %s' % (s['stage'], s['id'], short(f))
            x = ea10.get(k)
            if x is not None:
                line += '  | 110:%s' % ('same' if x['sha1'] == s['sha1'] else 'changed')
            y = evr.get(k)
            if y is not None:
                tv = text(dvr, y)
                fv = facts(y, tv)
                same = x is not None and x['sha1'] == y['sha1']
                if same:
                    line += '  | VR: identical to 110'
                    summary[(name.split(':')[0], 'vr-identical')] += 1
                else:
                    sim = difflib.SequenceMatcher(None, ops(text(d110, x)) if x else [], ops(tv), autojunk=False).ratio()
                    line += '  | VR(sim %.2f): %s' % (sim, short(fv))
                    summary[(name.split(':')[0], 'vr-changed')] += 1
            if len(g111[g]) > 12 and not want:
                continue
            print(line)
        if len(g111[g]) > 12 and not want:
            st = Counter(s['stage'] for s in g111[g])
            print('  (%d shaders %s; pass the group number to list them)' % (len(g111[g]), dict(st)))
    print('\n# summary', dict(summary))


if __name__ == '__main__':
    main()
