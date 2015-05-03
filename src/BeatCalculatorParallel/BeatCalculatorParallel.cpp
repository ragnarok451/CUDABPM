#include <iostream>
#include <stdio.h>
#include <mpg123.h>
#include <kiss_fftr.h>
#include "BeatCalculatorParallel.h"

BeatCalculatorParallel::BeatCalculatorParallel() {
    printf("Initialized Beat Calculator\n");
}

BeatCalculatorParallel::~BeatCalculatorParallel() {
    printf("Terminated Beat Calculator\n");
}

void BeatCalculatorParallel::cleanup(mpg123_handle* mh) {
    mpg123_close(mh);
    mpg123_delete(mh);
    mpg123_exit();
}

/*
 * Reads the values in the mp3 file indicated, and
 * stores them in the a and b arrays.
 * @Params: song - file path of the mp3 file
 *          a - holds left ear data
 *          b - holds right ear data
 */
int BeatCalculatorParallel::readMP3(char* song, unsigned short* sample) {
    mpg123_handle *mh = NULL;
    int err = MPG123_OK;
    int channels = 0, encoding = 0;
    long rate;
    err = mpg123_init();
    if (err != MPG123_OK || (mh = mpg123_new(NULL, &err)) == NULL) {
        fprintf(stderr, "Basic setup is going bad: %s\n", mpg123_plain_strerror(err));
        cleanup(mh);
        return -1;
    }

    mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_FORCE_FLOAT, 0);
    if (mpg123_open(mh, song) != MPG123_OK ||
            mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK) {
        fprintf(stderr, "Trouble with mpg123: %s\n", mpg123_strerror(mh));
        cleanup(mh);
        return -1;
    }

    if (encoding != MPG123_ENC_SIGNED_16 && encoding != MPG123_ENC_FLOAT_32) {
        cleanup(mh);
        fprintf(stderr, "Bad encoding! 0x%x!\n", encoding);
        return -1;
    }

    // Ensure format does not change
    mpg123_format_none(mh);
    mpg123_format(mh, rate, channels, encoding);

    //Read mp3
    size_t buffer_size = mpg123_length(mh);
    unsigned short* buffer = (unsigned short*)malloc(sizeof(unsigned short) * buffer_size);
    size_t done = 0;

    if (mpg123_read(mh, (unsigned char*)buffer, buffer_size, &done) != MPG123_OK) {
        cleanup(mh);
        fprintf(stderr, "Read went wrong\n");
        return -1;
    }

    // Extract 5 second sample
    int max_freq = 4096;
    int sample_size = 2.2*2*max_freq;

    // Calculate sample indices
    int start = buffer_size/2 - sample_size/2;

    memcpy(sample, buffer + start, sample_size * sizeof(unsigned short));

    free(buffer);

    cleanup(mh);

    return 0;
}

void BeatCalculatorParallel::fftrArray(unsigned short* sample, int size, kiss_fft_cpx* out) {
    kiss_fft_scalar in[size];
    kiss_fftr_cfg cfg;

    int i;

    if ((cfg = kiss_fftr_alloc(size, 0, NULL, NULL)) == NULL) {
        printf("Not enough memory to allocate fftr!\n");
        exit(-1);
    }

    for (int i = 0; i < size; i++) {
        in[i] = sample[i];
    }
    kiss_fftr(cfg, in, out);
    free(cfg);
}

void BeatCalculatorParallel::fftArray(unsigned short* sample, int size, kiss_fft_cpx* out) {
  kiss_fft_cpx in[size/2];
  kiss_fft_cfg cfg;
  int i;

  if ((cfg = kiss_fft_alloc(size/2, 0, NULL, NULL)) == NULL) {
    printf("Not Enough Memory?!?");
    exit(-1);
  }

  //set real components to one side of stereo input, complex to other
  for(i=0; i < size; i+=2) {
    in[i/2].r = sample[i];
    in[i/2].i = sample[i+1];
  }

  kiss_fft(cfg, in, out);
  free(cfg);

}

int BeatCalculatorParallel::combfilter(kiss_fft_cpx* fft_array, int size, int sample_size) {
    unsigned short AmpMax = 65535;
    int E[30];
    // Iterate through all possible BPMs
    for (int i = 0; i < 30; i++) {
        int BPM = 60 + i * 5;
        int Ti = 60 * 44100/BPM;
        unsigned short l[sample_size];
        for (int k = 0; k < sample_size; k+=2) {
            if ((k % Ti) == 0) {
                l[k] = AmpMax;
                l[k+1] = AmpMax;
            }
            else {
                l[k] = 0;
                l[k+1] = 0;
            }
        }
        kiss_fft_cpx out[sample_size/2];

        fftrArray(l, sample_size, out);
        E[i] = 0;
        for (int k = 0; k < sample_size/2; k++) {
            int a = fft_array[k].r * out[k].r - fft_array[k].i * out[k].i;
            int b = fft_array[k].r * out[k].i + fft_array[k].i * out[k].r;
            E[i] += a * a + b * b;
        }
    }

    //Calculate max of E[k]
    int max = -1;
    int index = -1;
    for (int i = 0; i < 30; i++) {
        if (E[i] > max) {
            max = E[i];
            index = i;
        }
    }
    return 60 + index * 5;
}

// Virtual CUDA functions
void cudaTest();
int cudaFFT(unsigned short* sample, int size, kiss_fft_cpx* out);

/* detect_beat
 * Returns the BPM of the given mp3 file
 * @Params: s - the path to the desired mp3
 */
int BeatCalculatorParallel::detect_beat(char* s) {

    // Cuda test
    cudaTest();

    // Step 1: Get a 5-second sample of our desired mp3
    // Assume the max frequency is 4096
    int max_freq = 4096;
    int sample_size = 2.2 * 2 * max_freq; //This is the sample length of our 5 second snapshot

    // Load mp3
    unsigned short* sample = (unsigned short*)malloc(sizeof(unsigned short) * sample_size);
    readMP3(s, sample);
    //for (int i = 0; i < sample_size; i++) {
    //    printf("Element %i: %i\n", i, sample[i]);
    //}

    // Step 2: Differentiate
    unsigned short* differentiated_sample = (unsigned short*)malloc(sizeof(unsigned short) * sample_size);
    int Fs = 44100;
    differentiated_sample[0] = sample[0];
    for (int i = 1; i < sample_size - 1; i++) {
        differentiated_sample[i] = Fs * (sample[i+1]-sample[i-1])/2; //TODO: Look here if this is messing up
    }
    differentiated_sample[sample_size - 1] = sample[sample_size-1];

    // Step 3: Compute the FFT
    kiss_fft_cpx out[sample_size/2+1];
    kiss_fft_cpx outCuda[sample_size/2+1];

    printf("Cuda FFT start\n");
    cudaFFT(sample, sample_size, outCuda);
    printf("CUDA FFT Finish\n");

    printf("Normal FFT Start\n");
    fftrArray(sample, sample_size, out);
    printf("Normal FFT Finish\n");

    printf("Checking for differences...\n");
    for (int i = 0; i < sample_size/2+1; i++) {
        if (outCuda[i].r != out[i].r || outCuda[i].i != out[i].i) {
            printf("Difference at index %i\n", i);
            printf("OutCuda: %f %f, Out: %f %f\n", outCuda[i].r, outCuda[i].i, out[i].r, out[i].i);
            break;
        }
    }

    //for (int i = 0; i < sample_size / 2; i++)
    //  printf("out[%2zu] = %+f , %+f\n", i, out[i].r, out[i].i);

    printf("Combfilter performing...\n");

    int BPM = combfilter(out, sample_size / 2, sample_size);

    printf("Final BPM: %i\n", BPM);

    // Step 4: Generate Sub-band array values
    free(sample);

    return BPM;
}

// Replicates the "control" part of the Matlab code
int BeatCalculatorParallel::control() {
    // Step 1 : Get a 5-second sample of our desired mp3

    // Assume that we are at
    return 0;
}