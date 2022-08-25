#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <alsa/asoundlib.h>

//Directory for the input audio file.
#define AUDIO_FILE_DIR "/home/username/Music/file.raw"

//Descriptor for the audio output device.
//Type "default" to use system's default audio output.
#define AUDIO_DEV "plughw:0,0"

#define BUFFER_SIZE 65536 //The size of buffer in samples

//The delay time (in number of samples) between each element of the equation.
#define DSP_N_DELAY 132
//Number of previous input samples to be used in the equation
#define DSP_M_FACTOR 3 
//Number of previous output samples to be used in the equation
#define DSP_N_FACTOR 3 
//Multipliers of input samples (current input sample + m factor previous input samples)
#define DSP_B_FACTOR (new float[DSP_M_FACTOR + 1] {0.6f, 0.3f, 0.1f, 0.05f}) 
//Multipliers of n factor previous output samples
#define DSP_A_FACTOR (new float[DSP_N_FACTOR] {0.6f, 0.3f, 0.1f})

#define BUFFER_SIZE_BYTES (2*BUFFER_SIZE)
#define BUFFER_SIZE_PER_CHANNEL (BUFFER_SIZE/2)

#define SAMPLE_MAX_VALUE (32767)
#define SAMPLE_MIN_VALUE (-32768)

//Threads
std::thread loadthread;
std::thread playthread;

//Audio File
std::fstream audio_file;
unsigned int file_size = 0;
unsigned int file_pos = 0;

//Audio Device
snd_pcm_t *audio_dev = NULL;
snd_pcm_uframes_t n_frames;
unsigned int audio_buffer_size = 0;
unsigned int buffer_n_div = 1;
short **pp_startpoint = NULL;

//Static Buffers
short *buffer_input_0 = NULL;
short *buffer_input_1 = NULL;
short *buffer_output_0 = NULL;
short *buffer_output_1 = NULL;
short *buffer_output_2 = NULL;
short *buffer_output_3 = NULL;

//Dynamic Buffers
short *curr_in = NULL; //Current input buffer
short *prev_in = NULL; //Previous input buffer
short *load_out = NULL; //Current output buffer (load)
short *play_out = NULL; //Current output buffer (play)
short *prev_out = NULL; //Previous output buffer (load)

unsigned int curr_buf_cycle = 0;

short *input_summing = NULL;
short *output_summing = NULL;

float *b_factor = NULL;
float *a_factor = NULL;

int n_sample = 0;

bool stop = false;

void update_buf_cycle(void)
{
  if(curr_buf_cycle == 3) curr_buf_cycle = 0;
  else curr_buf_cycle++;
  
  return;
}

void buffer_remap(void)
{
  switch(curr_buf_cycle)
  {
    case 0:
      curr_in = buffer_input_0;
      prev_in = buffer_input_1;
      load_out = buffer_output_0;
      play_out = buffer_output_2;
      prev_out = buffer_output_3;
      break;
      
    case 1:
      curr_in = buffer_input_1;
      prev_in = buffer_input_0;
      load_out = buffer_output_1;
      play_out = buffer_output_3;
      prev_out = buffer_output_0;
      break;
      
    case 2:
      curr_in = buffer_input_0;
      prev_in = buffer_input_1;
      load_out = buffer_output_2;
      play_out = buffer_output_0;
      prev_out = buffer_output_1;
      break;
      
    case 3:
      curr_in = buffer_input_1;
      prev_in = buffer_input_0;
      load_out = buffer_output_3;
      play_out = buffer_output_1;
      prev_out = buffer_output_2;
      break;
  }
  
  return;
}

void buffer_load(void)
{
  if(file_pos >= file_size)
  {
    stop = true;
    return;
  }
  
  audio_file.seekg(file_pos);
  audio_file.read((char*) curr_in, BUFFER_SIZE_BYTES);
  file_pos += BUFFER_SIZE_BYTES;
  return;
}

void buffer_play(void)
{
  unsigned int n_div = 0;
  int n_return = 0;
  
  while(n_div < buffer_n_div)
  {
    n_return = snd_pcm_writei(audio_dev, pp_startpoint[n_div], n_frames);
    if(n_return == -EPIPE) snd_pcm_prepare(audio_dev);
    
    n_div++;
  }
  
  return;
}

void load_input_summing(void)
{
  int n_terms = 1;
  int n_delay = 0;
  
  float l_input_sumf = b_factor[0]*curr_in[2*n_sample];
  float r_input_sumf = b_factor[0]*curr_in[2*n_sample + 1];
  
  while(n_terms <= DSP_M_FACTOR)
  {
    n_delay = n_terms*DSP_N_DELAY;
    if(n_sample < n_delay)
    {
      l_input_sumf += b_factor[n_terms]*prev_in[BUFFER_SIZE - 2*(n_delay - n_sample)];
      r_input_sumf += b_factor[n_terms]*prev_in[BUFFER_SIZE - 2*(n_delay - n_sample) + 1];
    }
    else
    {
      l_input_sumf += b_factor[n_terms]*curr_in[2*(n_sample - n_delay)];
      r_input_sumf += b_factor[n_terms]*curr_in[2*(n_sample - n_delay) + 1];
    }
    
    n_terms++;
  }
  
  int l_input_sum = roundf(l_input_sumf);
  int r_input_sum = roundf(r_input_sumf);
  
  if((l_input_sum < SAMPLE_MAX_VALUE) && (l_input_sum > SAMPLE_MIN_VALUE)) input_summing[0] = l_input_sum;
  else if(l_input_sum >= SAMPLE_MAX_VALUE) input_summing[0] = SAMPLE_MAX_VALUE;
  else if(l_input_sum <= SAMPLE_MIN_VALUE) input_summing[0] = SAMPLE_MIN_VALUE;
  
  if((r_input_sum < SAMPLE_MAX_VALUE) && (r_input_sum > SAMPLE_MIN_VALUE)) input_summing[1] = r_input_sum;
  else if(r_input_sum >= SAMPLE_MAX_VALUE) input_summing[1] = SAMPLE_MAX_VALUE;
  else if(r_input_sum <= SAMPLE_MIN_VALUE) input_summing[1] = SAMPLE_MIN_VALUE;
  
  return;
}

void load_output_summing(void)
{
  int n_terms = 1;
  int n_delay = 0;
  
  float l_output_sumf = 0.0f;
  float r_output_sumf = 0.0f;
  
  while(n_terms <= DSP_N_FACTOR)
  {
    n_delay = n_terms*DSP_N_DELAY;
    if(n_sample < n_delay)
    {
      l_output_sumf += a_factor[n_terms - 1]*prev_out[BUFFER_SIZE - 2*(n_delay - n_sample)];
      r_output_sumf += a_factor[n_terms - 1]*prev_out[BUFFER_SIZE - 2*(n_delay - n_sample) + 1];
    }
    else
    {
      l_output_sumf += a_factor[n_terms - 1]*load_out[2*(n_sample - n_delay)];
      r_output_sumf += a_factor[n_terms - 1]*load_out[2*(n_sample - n_delay) + 1];
    }
    
    n_terms++;
  }
  
  int l_output_sum = roundf(l_output_sumf);
  int r_output_sum = roundf(r_output_sumf);
  
  if((l_output_sum < SAMPLE_MAX_VALUE) && (l_output_sum > SAMPLE_MIN_VALUE)) output_summing[0] = l_output_sum;
  else if(l_output_sum >= SAMPLE_MAX_VALUE) output_summing[0] = SAMPLE_MAX_VALUE;
  else if(l_output_sum <= SAMPLE_MIN_VALUE) output_summing[0] = SAMPLE_MIN_VALUE;
  
  if((r_output_sum < SAMPLE_MAX_VALUE) && (r_output_sum > SAMPLE_MIN_VALUE)) output_summing[1] = r_output_sum;
  else if(r_output_sum >= SAMPLE_MAX_VALUE) output_summing[1] = SAMPLE_MAX_VALUE;
  else if(r_output_sum <= SAMPLE_MIN_VALUE) output_summing[1] = SAMPLE_MIN_VALUE;
  
  return;
}

void run_dsp(void)
{
  n_sample = 0;
  while(n_sample < BUFFER_SIZE_PER_CHANNEL)
  {
    load_input_summing();
    load_output_summing();
    load_out[2*n_sample] = input_summing[0] - output_summing[0];
    load_out[2*n_sample + 1] = input_summing[1] - output_summing[1];
    
    n_sample++;
  }
  
  return;
}

void load_startpoints(void)
{
  pp_startpoint[0] = play_out;
  unsigned int n_div = 1;
  
  while(n_div < buffer_n_div)
  {
    pp_startpoint[n_div] = &play_out[n_div*audio_buffer_size];
    n_div++;
  }
  
  return;
}

void loadthread_proc(void)
{
  buffer_load();
  run_dsp();
  update_buf_cycle();
  return;
}

void playthread_proc(void)
{
  load_startpoints();
  buffer_play();
  return;
}

void buffer_preload(void)
{
  curr_buf_cycle = 0;
  buffer_remap();
  
  buffer_load();
  run_dsp();
  update_buf_cycle();
  buffer_remap();
  
  buffer_load();
  run_dsp();
  update_buf_cycle();
  buffer_remap();
  
  return;
}

void playback(void)
{
  buffer_preload();
  while(!stop)
  {
    playthread = std::thread(playthread_proc);
    loadthread = std::thread(loadthread_proc);
    loadthread.join();
    playthread.join();
    buffer_remap();
  }
  
  return;
}

void buffer_malloc(void)
{
  audio_buffer_size = 2*n_frames;
  if(audio_buffer_size < BUFFER_SIZE) buffer_n_div = BUFFER_SIZE/audio_buffer_size;
  else buffer_n_div = 1;
  
  pp_startpoint = (short**) malloc(buffer_n_div*sizeof(short*));
  
  input_summing = (short*) malloc(2*sizeof(short));
  output_summing = (short*) malloc(2*sizeof(short));
  
  b_factor = DSP_B_FACTOR;
  a_factor = DSP_A_FACTOR;
  
  buffer_input_0 = (short*) malloc(BUFFER_SIZE_BYTES);
  buffer_input_1 = (short*) malloc(BUFFER_SIZE_BYTES);
  buffer_output_0 = (short*) malloc(BUFFER_SIZE_BYTES);
  buffer_output_1 = (short*) malloc(BUFFER_SIZE_BYTES);
  buffer_output_2 = (short*) malloc(BUFFER_SIZE_BYTES);
  buffer_output_3 = (short*) malloc(BUFFER_SIZE_BYTES);
  
  memset(buffer_input_0, 0, BUFFER_SIZE_BYTES);
  memset(buffer_input_1, 0, BUFFER_SIZE_BYTES);
  memset(buffer_output_0, 0, BUFFER_SIZE_BYTES);
  memset(buffer_output_1, 0, BUFFER_SIZE_BYTES);
  memset(buffer_output_2, 0, BUFFER_SIZE_BYTES);
  memset(buffer_output_3, 0, BUFFER_SIZE_BYTES);
  
  return;
}

void buffer_free(void)
{
  free(pp_startpoint);
  
  free(input_summing);
  free(output_summing);
  
  free(b_factor);
  free(a_factor);
  
  free(buffer_input_0);
  free(buffer_input_1);
  free(buffer_output_0);
  free(buffer_output_1);
  free(buffer_output_2);
  free(buffer_output_3);
  
  return;
}

bool open_audio_file(void)
{
  audio_file.open(AUDIO_FILE_DIR, std::ios_base::in);
  if(audio_file.is_open())
  {
    audio_file.seekg(0, audio_file.end);
    file_size = audio_file.tellg();
    file_pos = 0;
    audio_file.seekg(file_pos);
    return true;
  }
  
  return false;
}

bool audio_hw_init(void)
{
  int n_return = 0;
  snd_pcm_hw_params_t *hw_params = NULL;
  
  n_return = snd_pcm_open(&audio_dev, AUDIO_DEV, SND_PCM_STREAM_PLAYBACK, 0);
  if(n_return < 0)
  {
    std::cout << "Error opening audio device\n";
    return false;
  }
  
  snd_pcm_hw_params_malloc(&hw_params);
  snd_pcm_hw_params_any(audio_dev, hw_params);
  
  n_return = snd_pcm_hw_params_set_access(audio_dev, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
  if(n_return < 0)
  {
    std::cout << "Error setting access to read/write interleaved\n";
    return false;
  }
  
  n_return = snd_pcm_hw_params_set_format(audio_dev, hw_params, SND_PCM_FORMAT_S16_LE);
  if(n_return < 0)
  {
    std::cout << "Error setting format to signed 16bit little-endian\n";
    return false;
  }
  
  n_return = snd_pcm_hw_params_set_channels(audio_dev, hw_params, 2);
  if(n_return < 0)
  {
    std::cout << "Error setting channels to stereo\n";
    return false;
  }
  
  unsigned int sample_rate = 44100;
  n_return = snd_pcm_hw_params_set_rate_near(audio_dev, hw_params, &sample_rate, 0);
  if(n_return < 0 || sample_rate < 44100)
  {
    std::cout << "Could not set sample rate to 44100 Hz\nAttempting to set sample rate to 48000 Hz\n";
    sample_rate = 48000;
    n_return = snd_pcm_hw_params_set_rate_near(audio_dev, hw_params, &sample_rate, 0);
    if(n_return < 0 || sample_rate < 48000)
    {
      std::cout << "Error setting sample rate\n";
      return false;
    }
  }
  
  n_return = snd_pcm_hw_params(audio_dev, hw_params);
  if(n_return < 0)
  {
    std::cout << "Error setting hardware parameters\n";
    return false;
  }
  
  snd_pcm_hw_params_get_period_size(hw_params, &n_frames, 0);
  snd_pcm_hw_params_free(hw_params);
  return true;
}

int main(int argc, char **argv)
{
  if(!audio_hw_init())
  {
    std::cout << "Error code: " << errno << "\nTerminated\n";
    return 0;
  }
  std::cout << "Audio hardware initialized\n";
  
  if(!open_audio_file())
  {
    std::cout << "Error opening audio file\nError code: " << errno << "\nTerminated\n";
    return 0;
  }
  std::cout << "Audio file is open\n";
  
  buffer_malloc();
  
  std::cout << "Playback started\n";
  playback();
  std::cout << "Playback finished\n";
  
  audio_file.close();
  snd_pcm_drain(audio_dev);
  snd_pcm_close(audio_dev);
  buffer_free();
  
  std::cout << "Terminated\n";
  return 0;
}
