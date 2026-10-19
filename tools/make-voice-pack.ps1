<#
  Makes the spoken prompts for mtyaSampler's recording assistant (Create page),
  using the text-to-speech voice built into Windows. Run it once:

      powershell -ExecutionPolicy Bypass -File tools\make-voice-pack.ps1

  Options:
      -List              show the voices installed on this PC
      -Voice "Name"      use a specific voice (from -List)
      -Rate 1            speak faster (-10 .. 10)

  The WAV files go to Documents\mtyaSampler\VoicePack. The plugin loads them when it starts,
  and plays the prompts through its own output (so you hear them in the DAW, like a guide track).
  Without a voice pack, the plugin window speaks the prompts with the system voice instead.
  You can also replace any file with your own recording (same file name, mono or stereo WAV).
#>
param([switch]$List, [string]$Voice = "", [int]$Rate = 1)

Add-Type -AssemblyName System.Speech
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer

if ($List) {
    $synth.GetInstalledVoices() | ForEach-Object { $_.VoiceInfo.Name }
    return
}
if ($Voice) { $synth.SelectVoice($Voice) }
$synth.Rate = $Rate

$out = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'mtyaSampler\VoicePack'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(44100,
    [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
    [System.Speech.AudioFormat.AudioChannel]::Mono)

# file name (word the plugin looks for)  →  what the voice actually says
$words = [ordered]@{
    a = 'Ay'; b = 'Bee'; c = 'See'; d = 'Dee'; e = 'Ee'; f = 'Eff'; g = 'Gee'
    sharp = 'sharp'; minus = 'minus'
    zero = 'zero'; one = 'one'; two = 'two'; three = 'three'; four = 'four'; five = 'five'
    six = 'six'; seven = 'seven'; eight = 'eight'; nine = 'nine'; ten = 'ten'
    eleven = 'eleven'; twelve = 'twelve'; thirteen = 'thirteen'; fourteen = 'fourteen'; fifteen = 'fifteen'
    sixteen = 'sixteen'; seventeen = 'seventeen'; eighteen = 'eighteen'; nineteen = 'nineteen'; twenty = 'twenty'
    normal = 'normal'; very = 'very'; soft = 'soft'; medium = 'medium'; loud = 'loud'
    take = 'take'; sound = 'sound'
}

foreach ($key in $words.Keys) {
    $synth.SetOutputToWaveFile((Join-Path $out "$key.wav"), $format)
    $synth.Speak($words[$key])
}
$synth.SetOutputToNull()
$synth.Dispose()

Write-Host "Voice pack written to $out ($($words.Count) words). Restart the plugin to load it."
