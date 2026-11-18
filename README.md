# fo4flatvr plugin

It seemed like a ~~better~~ way to be compatible with new/other mods than backporting every single thing onto that old openvr thing.

## What does it do?
It makes fallout 4 1.11.240 work in vr with openxr.

## Does this work and should I try it?
Yes. No.

## Why? does it perform bad?
No, it was fine for me. There's probably some tuning overhead though.

## Why is it here then?
It seemed like a waste of electricity not to.

## Could the same approach be made to work with 1.10.163?
Yes

## Did AI make this?
Yeah there's some deep dark display/openxr/porting stuff in here. There's no way I'd have been able to do unless I worked at it for a long time and I'd probably have lost interest.

## Could the whole game be played through with it with absolutely no crashes?
We're talking fallout 4 here, so no.

## Could the whole game be played through with it?
Yeah I'd say so from testing. Who knows though, fallout 4 probably is hiding a dirty trick.

## How long did it take for senor claude to make it?
A weekend.

## Is everything identical to the VR version?
No.

## How do you use it?
It's a F4SE dll. The usual way.

## How to build?
```
git clone https://github.com/herpaderpapotato/Fallout-4-Flat-2-VR
cd Fallout-4-Flat-2-VR
git submodule update --init --recursive
xmake f -p windows -a x64 -m releasedbg -y
xmake build -y fo4flatvr
```
That spits out the special dll into build\windows\x64\releasedbg

There's stuff in dist here too.

## What else do you need?
Special FRIK https://github.com/herpaderpapotato/Fallout-4-VR-Body/
To build that
```
python -I tools/gen.py
xmake f -p windows -a x64 -m releasedbg -y
xmake build -y FRIK        # build/windows/x64/releasedbg/FRIK.dll
```
