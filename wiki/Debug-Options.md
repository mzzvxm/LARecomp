# Debug Options

The game's retail build keeps a table of about 540 developer command-line options, registered as inactive stubs. LARecomp reactivates them: a text file lists the ones to turn on, and some performance cvars drive a few of them. Source: `src/mc_engine/hooks/dev_options.cpp`, `src/mc_engine/debug_options_table.inc`.

These options belong to the game, not to LARecomp, and the repository has no description for almost any of them. Names are listed below for completeness; effects are "undocumented" except where the code states one.

## `debug_options.txt`

Put the file in the process working directory. The code reads it from the current directory; the header comment of the source file says the executable folder, and the two are the same when the game is started from its own folder. It is read at startup.

Format:

- One option per line: `name` or `name=value`. A bare name means the value `1`.
- `#` and `;` start a comment, anywhere on the line. Blank lines are ignored.
- Whitespace around the name and value is trimmed.
- If the same option appears twice, the last one wins.

```text
# example
unlockall
showframerate
```

Names are matched exactly (case-sensitive) against the table below. An unknown name is logged and skipped:

```text
[DbgOpt] unknown option 'name'
```

Log lines for a good run:

```text
[DbgOpt] queued <name> = <value> (node+4 @ 0x........)
[DbgOpt] queued N option(s) from file, M unknown
[DbgOpt] applied node+4 @ 0x........ = <value>
```

Options are applied when the game registers them, before anything reads them. They are load-time settings; the source says a value written after boot would be too late. If the scratch memory cannot be made writable, all options are disabled: `[DbgOpt] cannot make scratch ... writable; dev options disabled`. If it fills up: `[DbgOpt] scratch full, dropping ...`.

## Options driven by cvars

These performance cvars are backed by a developer option. When the cvar is true the option is set to `1`. A file value for the same option is left alone while the cvar is off.

| Cvar | Option | Menu row |
|---|---|---|
| `perf_no_race_shadows` | `noraceshadows` | none |
| `perf_fast_vehicle_shadows` | `fastVehShadows` | GRAPHICS > PERFORMANCE > SHADOWS > CHEAP CAR SHADOW |
| `perf_no_impostors` | `noimpostors` | GRAPHICS > PERFORMANCE > SHADOWS > TREE IMPOSTORS |
| `perf_no_trees` | `notrees` | GRAPHICS > PERFORMANCE > SHADOWS > FOLIAGE |
| `perf_no_fullscreen_blur` | `nofsblur` | GRAPHICS > PERFORMANCE > RENDERING > SCREEN BLUR |

Notes from the code:

- `noimpostors` and `notrees` are load-time: they skip creating render targets and the whole prop parse. The cvar value is kept in sync every frame, so a change applies the next time that system loads (a district change or a world reload) rather than needing a process restart.
- `perf_no_shadows` is not in this list: it edits a render phase mask directly each frame and takes hold without a restart.
- The option `rearview` is forced on for the whole session so the rear view mirror's meshes load; showing and hiding the mirror is the job of the `rearview` cvar. See [Settings Reference](Settings-Reference#mclacamera-37).

## Option names

540 unique names, from `src/mc_engine/debug_options_table.inc`. The comment in that file says 541 stubs. All are undocumented in this repository apart from the six named above.

```text
addplayerwidgets  aimodvalue  aimodvalue1  aimodvalue2  aimodvalue3  aimodvalue4  aimodvalue5
aimodvalue6  aimodvalue7  aimodvalue8  AIOverrideTuning  aivperf  allagro  allambientcars
allambientpeds  allgrip  allroar  allsignalsgreen  allvirtual  allzone  ambientcarspawncenter
ambientlayerthread  ambientpedspawncenter  ambienttype  animmachineprint  animplayerasopponent
ao  appname  archive  assumelosonspawning  assumelosonunspawning  assumenolosonspawning
assumenolosonunspawning  ati  audio  audioocclusion  audiooutputchannels  audiorpfdir
audiotestslot  autokick  autoprogress  autorunpasses  bikefakehispeed  bikeforcelowturnsweep
blockuntilidle  bootscript  breakonaddr  breakonalloc  breakonbucket  bypasscutscenecam
camfocus  campos  capturestart  capturestop  car  car1  careerDebugLvl  carname  carName
carname1  cheapaudioeffects  cinefixedframe  cinerealtime  city  cleardiskcache  clearhddcache
collapsechild  colorexpbias  commandbuffer  controlcfg1  controlcfg2  cop  countscriptthreads
createchild  ctfAllBombs  ctfFlagLimit  ctfTimeLimit  cutsceneindex  cutsceneshadows
cyclegamefe  cyclelods  d3dsinglestep  damagespew  debugcamcuts  debugcharactercuts
debugcruisingroutes  debugoccluders  debugrailnet  debugshaders  deletesave  demo
devautotrans  devmanualtrans  devmenus  devonlycars  devonlyraces  disablebreakingimpulses
disablecarcommandbuffers  disablefemdriveranim  disableopponenthires  disablepreracetaunt
disablereadahead  disablesave  disableStatUpload  displaystreamingqueue  dongle
drawablecontainersize  drawcheckpoints  drawcorridor  drawdistance  drawfollow  drawobstacles
drawraceline  drawwindowclipplane  dumpleaks  dynmixerwidgets  ekg  emulatevehvis
enablehqrender  eventdebuginfo  extracarname  extrad3dmem  extraheapmem  extraoversample
ezdamage  fakedlcreboot  fakerez  fastdraw  fastrez  fastVehShadows  fixedSizeCarConfigExport
fixedtrafficboxlength  fixedtrafficboxwidth  flashdesignertraffic  force_aclass  force_bclass
force_cclass  force_dclass  forcebigsession  forcebootcd  forcecaminterest  forcediskcache
forcefemdriver  forceheadless  forceidentityface  forceidentitypose  forceplayeraudioocclusion
forcePowerupsOn  forcepreracetaunt  forcereadahead  forcesubtitles  forcezeropose  fpz
fragcachewarn  fragdebug  fragdebug2  fraggeo  fragresdir  fragtune  framelock  framerate
frametime  fullscreen  fullsoundhierarchy  fullstreamingspew  gfxcapturestart  gfxcapturestop
globalroaddensity  hdr  height  hidewindow  hudflash  hydraulics  ignoreazimuth  keepTelem
lang  latestaudio  layerDebug  layerHeapSanity  layout  layoutmode  leaktest  level
LoadAllCityPages  loadcustomcontrols  loadmaterials  loadreplay  localbank  localCtfTune
localexplosiontuning  localhost  locallighttuning  localMcNetTune  localpoweruptuning
localshaders  localsubtitles  localuimeshes  localuitune  logallocstack  logmemory  logopen
logstart  logstop  lowrescity  makeswf  manualtrans  marketing  maskneck  maxarcadeopponents
maxtexturesize  measure  memtunearchive  minimach  missingfilesarefatal  mission  modvalue
money  multisample  musicmanager  netAllowOneRacerRaces  netAutoConnect  netCxnMgrWarning
netDebug  netDebugPolicies  netdemo  netDisableAutoJoinOwnRace  netFakeDrop  netFakeLatency
nethost  netjoin  netLog  netLogHDD  netmaxpeers  netMaxPlayers  netNoDeltaComp  netPort
netPowerupsInRaces  netRandomHostMigrations  netRequestRaceDelay  netShowDebug  netTightLogon
netVersion  netwait  netWaitTimeForLoad  noaa  noai  noaiburnout  noainitro  noairevving
noalloccharinstondemand  noambientdamage  noambientlayer  noambientspawn  noambientunspawn
noao  noaudio  noaudioocclusion  noaudiorpfs  noaudiothread  noautomatictraffic
nobasecategory  nobikeshifting  noblendshapes  noburnout  nocamwidgets  nocardrivers  nocareer
nocarparts  noCharCloth  nocineanimpacks  nocinematics  nocitypaging  nocommandbuffers  nocpv
nocpvs  nocsprobeslope  nocutscenes  nodamage  nodevmenus  nodownloadablecontent
nodrawablecontainer  noeclecticmusic  noelectronicmusic  noEnvCloth  nofeedback
nofragarticulation  nofragbreak  nofreewayoptimization  nofsblur  nogaragewidgets  nogfxonly
nographics  nohangoutpeds  nohardrockmusic  nohdcaching  nohddcache  noheadless  nohiphopmusic
nohwtnl  noimpostors  noloadaudio  noloadmovie  nolod  nolookatoverlay  nomaskcameratrans
nomenutocam  nomixface  nomotionblur  nomousepointer  nomtstreamingqueue  nomusic  noocclusion
noparkedcars  noparticles  nopeds  nopoliceradio  nopowerup  noprops  nopvs  noraceshadows
noracestarter  noragdoll  norockmusic  nort  norubberband  nosave  noscripts  noshaderpatching
noshadows  nosharingrendertargetmem  nospecialabilities  nospeechmissingasset  nospuaudio
nospucollide  nossttrail  nostringBAM  noswapearly  notechnomusic  nothreadai  notitlestorage
notraffic  notrafficaudio  notrafficclearing  notrafficssd  notrees  notreeshadows
notriggeredintersections  notriggeredroaddensity  notriggeredtraffic  notuneparticles
notutorials  noumbra  novblank  nowaveslots  nowestrapmusic  nowind  nullhardware
numcontactgroups  numcontacts  numcontactspergroup  numlaps  numopponents  numviews
oneambient  oneofeachambient  oneprop  onlyproxyambients  oppcarname  oppcarname1  oppcarname2
oppcarname3  oppcarname4  oppcarname5  oppcarname6  oppdifficulty  opponent  opponentaudio
opponentcharname1  opponentcharname2  opponentcharname3  opponentcharname4  opponentcharname5
opponentcharname6  opponentdamage  opponentmod  orthoheight  outputLogFile  particleaudiospew
partLoadWait  path  pedaudioname  peds  pedsonline  physicsupdates  pixannotation
playercharname1  playercharname2  playercharname3  playercharname4  playercharname5
playercharname6  playercharname7  playercharname8  players  pneumatics
policespeechfullroadspew  powerconsole  preracecutscene  presentflags  pretendwheel
printmissioninfo  printracenames  prop  propaudiospew  props  ptxtexdict  purchasedflags  pvs
quitonleak  quitonload  racecopchase  raceeditor  racename  rag  ragAddr  ragviewer
randomseed  rawanimpack  rawcartype  rawlayout  rawlighting  rawMcUiClassTune  rawparticles
rawprops  rawrailnet  realtime  rearview  recordrace  recordracedir  recordracetimes
redeliver  regenfragdistricts  regenfrags  remotebank  rlDebug  rmptfxpath  rtspew  rubberband
savereplay  savevinyls  script  separatedevcams  shadows  shortreplay  showframerate
showmemory  showpickups  showstreammem  showstreamq  simplesimonly  snuCpu  spawnaroundcamera
spewhandlebardebug  spewsongnames  spuCityCull  spucollide  srgb  startPos  stateLoadEcho
staticTOD  stoponclosedroads  stringid  stringspew  subtitles  swapearly  synchronous
teammode  test16  test4xAA  testprop  testragdoll  testroute  testspeedculldriveranim
testtrailers  timeofday  topmost  tracesize  tuneagro  tunecharshaders  tunecheckpointcolors
tunegrip  tunelicenseplates  tunelocalcheckpoints  tuneprops  tunepulse  tunerims  tuneroar
tunexml  tunezone  twenty  uispew  umbra  unlimitedainitrous  unlimiteddamage
UnlimitedNitrous  unlimitednitrous  unlimitedspecial  unlockabilities  unlockall  unlockcareer
unlockcities  unlockcustomization  unlockdifficulty  unlockgarage  unlockinsanity  unlockparts
unlockperformance  unlockperfupgrades  unlockraces  unlockspecialabilities  unlockvehicles
usenightlod  useoldstreaming  useragecache  validatephysics  verifyallanimtracks  verifyanim
verifyblendshapes  verifyfaceanimtracks  vhsmspew  voicecapture  warnonbadrefs  weather  width
windowed  xaudiothreads  xlspenumbaseattempts  xlspenumdelayms  xlspenumgrowattempts
xlspsgfilter
```
