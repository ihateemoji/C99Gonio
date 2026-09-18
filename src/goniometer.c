/*
 * C99Gonio — CLAP entry point, DSP, ports, state
 *
 * Responsibilities of this file:
 *   - clap_entry / factory / plugin lifecycle
 *   - stereo audio pass-through
 *   - ring-buffer capture of (L,R) pairs for the GUI
 *   - CLAP state save/load (currently just magic/version)
 *
 * There are deliberately no parameters.  Display zoom is automatic.
 */

#include "goniometer.h"

void go_state_default(go_state_t *st) {
    /* Set the default state variables of the plug-in
        Inputs:
            <*go_state_t> - pointer to the instance of the plug-in's state */
    /* set the magic and version */
    memset(st, 0, sizeof(*st));
    st->magic   = GO_MAGIC;
    st->version = GO_VERSION;
}

void go_clamp(go_state_t *st) {
    /* Force every field of a state structure into a legal range.
       Inputs:
         <*go_state_t> - structure that may contain out-of-range values
       Outputs:
         All numeric fields are clamped or wrapped so that subsequent
         generation and playback code can assume valid data. */
    /* Nothing to clamp yet — kept for symmetry with other plug-ins. */
    (void)st;
}

static int write_all(const clap_ostream_t *s, const void *p, uint64_t n) {
    /* Function that writes given number of bytes to CLAP output stream.
       Inputs:
        <*clap_ostream_t> - output stream supplied by CLAP host
        <*void>           - address of the data to write
        <uint64_t>        - number of bytes to write
       Returns:
        <int>             - 1 if all bytes were written successfully
                            0 if the stream reported an error */
    const uint8_t *b = (const uint8_t *)p;
    uint64_t off = 0;
    while (off < n) {
        int64_t w = s->write(s, b + off, n - off);
        if (w <= 0) return 0;
        off += (uint64_t)w;
    }
    return 1;
}

static int read_all(const clap_istream_t *s, void *p, uint64_t n) {
    /* Function that reads given number of bytes from CLAP output stream.
       Inputs:
        <*clap_istream_t> - input stream supplied by the host
        <*void>           - pointer to the destination memory
        <uint64_t>        - number of bytes to read 
       Returns:
        <int>             - 1 if all bytes were read successfully
                            0 if the stream reported an error */
    uint8_t *b = (uint8_t *)p;
    uint64_t off = 0;
    while (off < n) {
        int64_t r = s->read(s, b + off, n - off);
        if (r <= 0) return 0;
        off += (uint64_t)r;
    }
    return 1;
}

static bool go_init(const clap_plugin_t *plugin) {
    /* Function that initialises the instance of the plug-in.
       Inputs:
        <*clap_plugin_t> - CLAP plug-in instance to be initialised
       Outputs:
        <bool>           - whether or not the initialisation was successful */
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    /* Cache host extensions we care about (may be NULL on minimal hosts). */
    plug->host_log = (const clap_host_log_t *)
        plug->host->get_extension(plug->host, CLAP_EXT_LOG);
    plug->host_params = (const clap_host_params_t *)
        plug->host->get_extension(plug->host, CLAP_EXT_PARAMS);
    plug->host_state = (const clap_host_state_t *)
        plug->host->get_extension(plug->host, CLAP_EXT_STATE);
    plug->host_gui = (const clap_host_gui_t *)
        plug->host->get_extension(plug->host, CLAP_EXT_GUI);
    plug->host_fd = (const clap_host_posix_fd_support_t *)
        plug->host->get_extension(plug->host, CLAP_EXT_POSIX_FD_SUPPORT);
    return true;
}

static void go_destroy(const clap_plugin_t *plugin) {
    /* Function that destroys the instance of the plug-in.
       Inputs:
        <*clap_plugin_t> - CLAP plug-in instance to be destroyed */
    free(plugin->plugin_data);
}

static bool go_activate(const clap_plugin_t *plugin, double sr,
                        uint32_t min_frames, uint32_t max_frames) {
    /* Function that activates the plug-in and begins processing.
       Inputs:
        <double> - host sample rate
        <uint32_t> - minimum audio block size
        <uint32_t> - maximum audio block size
       Outputs:
        <bool>     - whether or not the activation was successful */
    /* these are meaningless as this is not an audio plug-in */
    (void)min_frames;
    (void)max_frames;
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    plug->sr = sr > 0.0 ? sr : 44100.0;
    plug->active = 1;
    /* Clear analysis state so we do not show stale data after re-activate. */
    plug->scope_write = 0;
    plug->scope_count = 0;
    plug->disp_count  = 0;
    plug->auto_peak   = 0.01f;   /* small floor so scale starts sane */
    plug->corr = 0.f;
    plug->bal  = 0.f;
    plug->peak_l = plug->peak_r = 0.f;
    return true;
}

static void go_deactivate(const clap_plugin_t *plugin) {
    /* Function that deactivates the instance of the plug-in.
       Inputs:
        <*clap_plugin_t> - CLAP plug-in instance to be deactivated */
    ((go_plug_t *)plugin->plugin_data)->active = 0;
}

static bool go_start_processing(const clap_plugin_t *plugin) {
    /* Function that starts audio processing by the plug-in.
       Inputs:
        <*clap_plugin_t> - CLAP plug-in instance */
    ((go_plug_t *)plugin->plugin_data)->processing = 1;
    return true;
}

static void go_stop_processing(const clap_plugin_t *plugin) {
    /* Function that stops audio processing by the plug-in.
       Inputs:
        <*clap_plugin_t> - CLAP plug-in instance */
    ((go_plug_t *)plugin->plugin_data)->processing = 0;
}

static void go_reset(const clap_plugin_t *plugin) {
    /* Function that resets audio processing by the plug-in.
       Inputs:
        <*clap_plugin_t> - CLAP plug-in instance */
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    plug->scope_write = 0;
    plug->scope_count = 0;
    plug->disp_count  = 0;
    plug->auto_peak   = 0.01f;
    plug->corr = 0.f;
    plug->bal  = 0.f;
    plug->peak_l = plug->peak_r = 0.f;
}

static void go_on_main_thread(const clap_plugin_t *plugin) {
    /* Function that handles main thread work (none in our case).
       Inputs:
        <*clap_plugin_t> - CLAP plug-in instance */
    (void)plugin;
}

static uint32_t audio_ports_count(const clap_plugin_t *plugin,
                                                    bool is_input) {
    /* Function that returns the number of audio ports exposed by the plugin.
       Inputs:
        <*clap_plugin_t> - plugin instance
        <bool>           - whether the requested ports are inputs
       Returns:
        <uint32_t> - number of audio ports in the requested direction
       The plugin has one input and one output port -> very simple */
    (void)plugin;
    (void)is_input;
    return 1;
}

static bool audio_ports_get(const clap_plugin_t *plugin, uint32_t index,
                            bool is_input, clap_audio_port_info_t *info) {
    /* Function that describes one of the plugin's audio ports.
       Inputs:
        <*clap_plugin_t>             - plugin instance
        <uint32_t>                   - requested port index
        <bool>                       - whether the requested port is an input
        <*clap_audio_port_info_t>    - structure to fill with port information
       Returns:
        <bool>                       - true if the requested port exists */
    (void)plugin;
    if (index != 0) return false;
    /* describe our single in/out stereo port in each direction */
    memset(info, 0, sizeof(*info));
    info->id = 0;
    snprintf(info->name, sizeof(info->name), "%s", is_input ? "In" : "Out");
    info->channel_count = 2;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

/* Interface used by the host to enumerate the plug-in's audio ports. */
static const clap_plugin_audio_ports_t s_audio_ports = {
    .count = audio_ports_count,
    .get   = audio_ports_get
};

static uint32_t params_count(const clap_plugin_t *plugin) {
    /* Function that returns the number of parameters exposed by the plug-in.
       Inputs:
        <*clap_plugin_t> - plug-in instance
       Returns:
        <uint32_t> - total number of plug-in parameters */
    (void)plugin;
    return 0;
}

static bool params_info(const clap_plugin_t *plugin, uint32_t index,
                                                clap_param_info_t *info) {
    /* Function that describes one of the plug-in's parameters.
       Inputs:
        <*clap_plugin_t>         - plug-in instance
        <uint32_t>               - requested parameter index
        <*clap_param_info_t>     - structure to fill with parameter
                                                            information
       Returns:
        <bool> - true if the requested parameter exists */
    (void)plugin;
    (void)index;
    (void)info;
    return false;
}

static bool params_get_value(const clap_plugin_t *plugin,
                                        clap_id id, double *out) {
    /* Function that returns the current value of a plug-in parameter.
       Inputs:
        <*clap_plugin_t> - plug-in instance
        <clap_id>        - identifier of the requested parameter
        <*double>        - location to receive the parameter value
       Returns:
        <bool> - true after the current value has been written to out */
    (void)plugin;
    (void)id;
    (void)out;
    return false;
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id,
                                 double value, char *display, uint32_t size) {
    /* Function that converts a numeric parameter value into display text.
        Inputs:
         <*clap_plugin_t> - plug-in instance
         <clap_id>        - identifier of the parameter
         <double>         - numeric parameter value
         <char *>         - output text buffer
         <uint32_t>       - capacity of the output buffer
        Returns:
         <bool> - true if the value was converted successfully */
    (void)plugin;
    (void)id;
    (void)value;
    (void)display;
    (void)size;
    return false;
}

static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id,
                                        const char *display, double *value) {
    /* Function that converts parameter text into a numeric value.
       Inputs:
        <*clap_plugin_t> - plug-in instance
        <clap_id>        - identifier of the parameter
        <const char *>   - text to convert
        <*double>        - location to receive the converted value
       Returns:
        <bool> - true if text was provided and converted */
    (void)plugin;
    (void)id;
    (void)display;
    (void)value;
    return false;
}

static void params_flush(const clap_plugin_t *plugin,
                         const clap_input_events_t *in,
                         const clap_output_events_t *out) {
    /* Function that flushes parameter changes and regenerates the pattern.
        Inputs:
         <*clap_plugin_t>          - plug-in instance
         <*clap_input_events_t>    - host input event list
         <*clap_output_events_t>   - host output event list */
    (void)plugin;
    (void)in;
    (void)out;
}

/* Interface used by the host to access plug-in parameters. */
static const clap_plugin_params_t s_params = {
    .count         = params_count,
    .get_info      = params_info,
    .get_value     = params_get_value,
    .value_to_text = params_value_to_text,
    .text_to_value = params_text_to_value,
    .flush         = params_flush
};

static bool state_save(const clap_plugin_t *plugin,
                            const clap_ostream_t *stream) {
    /* Function that saves the plug-in state to the host stream.
        Inputs:
         <*clap_plugin_t>  - plug-in instance
         <*clap_ostream_t> - output stream supplied by the host
        Returns:
         <bool> - true if the complete state was written successfully */
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    return write_all(stream, &plug->st, sizeof(plug->st));
}

static bool state_load(const clap_plugin_t *plugin,
                            const clap_istream_t *stream) {
    /* Function that loads and validates the plug-in state
                                            from the host stream.
       Inputs:
        <*clap_plugin_t>  - plug-in instance
        <*clap_istream_t> - input stream supplied by the host
       Returns:
        <bool> - true if the state was read, validated, 
                                        and applied successfully */
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    go_state_t tmp;
    if (!read_all(stream, &tmp, sizeof(tmp))) return false;
    if (tmp.magic != GO_MAGIC) return false;
    plug->st = tmp;
    go_clamp(&plug->st);
    return true;
}

/* Interface used by the host to save and load plugin state. */
static const clap_plugin_state_t s_state = {
    .save = state_save,
    .load = state_load
};

static clap_process_status go_process(const clap_plugin_t *plugin,
                                      const clap_process_t *process) {
    /* Function that processes one audio block.
       Inputs:
        <*clap_plugin_t>   - plug-in instance
        <*clap_process_t>  - current processing block
       Returns:
        <clap_process_status> - processing status returned to the host */
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    /* get number of frames from the host */
    uint32_t frames = process->frames_count;
    /* take care if for some reason host gave us no ports */
    if (!process->audio_inputs || !process->audio_outputs ||
        process->audio_inputs_count < 1 || process->audio_outputs_count < 1) {
        return CLAP_PROCESS_CONTINUE;
    }
    /* otherwise grab pointers to left and right ports */
    const float *inL = process->audio_inputs[0].data32
                           ? process->audio_inputs[0].data32[0] : NULL;
    const float *inR = process->audio_inputs[0].data32 &&
                       process->audio_inputs[0].channel_count > 1
                           ? process->audio_inputs[0].data32[1] : inL;
    float *outL = process->audio_outputs[0].data32
                      ? process->audio_outputs[0].data32[0] : NULL;
    float *outR = process->audio_outputs[0].data32 &&
                  process->audio_outputs[0].channel_count > 1
                      ? process->audio_outputs[0].data32[1] : outL;
    if (!inL || !outL) {
        return CLAP_PROCESS_CONTINUE;
    }
    /* Pure pass-through.  All analysis (scope, corr, bal, auto_peak)
       lives in the GUI thread.  We only feed raw (L,R) pairs. */
    /* we do not want to flood the gui with points, so ensure we never write
                                                        more that 64 frames */
    uint32_t step = frames > 64 ? frames / 64 : 1;
    if (step < 1) step = 1;
    /* loop over incoming frames */
    for (uint32_t i = 0; i < frames; i++) {
        float L = inL[i];
        float R = inR ? inR[i] : L;
        /* direct pass through to the output */
        outL[i] = L;
        if (outR) outR[i] = R;
        /* on every step, we sample our signal for the gui to draw */
        if ((i % step) == 0) {
            go_point_t *pt = &plug->scope[plug->scope_write];
            pt->L = L;
            pt->R = R;
            plug->scope_write = (plug->scope_write + 1) % GO_SCOPE_LEN;
            if (plug->scope_count < GO_SCOPE_LEN)
                plug->scope_count++;
        }
    }
    return CLAP_PROCESS_CONTINUE;
}

/* ========================================================================
 * Extensions + descriptor + factory + entry
 * ======================================================================== */

static const void *go_get_extension(const clap_plugin_t *plugin, const char *id)
{
    (void)plugin;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))      return &s_audio_ports;
    if (!strcmp(id, CLAP_EXT_PARAMS))           return &s_params;
    if (!strcmp(id, CLAP_EXT_STATE))            return &s_state;
    if (!strcmp(id, CLAP_EXT_GUI))              return &go_gui_ext;
    if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &go_posix_fd_ext;
    return NULL;
}

static const char *s_features[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_STEREO,
    CLAP_PLUGIN_FEATURE_ANALYZER,
    CLAP_PLUGIN_FEATURE_UTILITY,
    NULL
};

static const clap_plugin_descriptor_t s_desc = {
    .clap_version = CLAP_VERSION_INIT,
    .id           = "com.ihateemoji.c99gonio",
    .name         = "C99Gonio",
    .vendor       = "ihateemoji",
    .url          = "",
    .manual_url   = "",
    .support_url  = "",
    .version      = "0.0.1",
    .description  = "Minimal C99 stereo goniometer.",
    .features     = s_features
};

static const clap_plugin_t *go_create(const clap_host_t *host)
{
    go_plug_t *plug = (go_plug_t *)calloc(1, sizeof(go_plug_t));
    if (!plug) return NULL;

    go_state_default(&plug->st);
    plug->host = host;
    plug->sr = 44100.0;
    plug->xfd = -1;
    plug->timer_fd = -1;
    plug->back = None;
    plug->auto_peak = 0.01f;

    plug->plugin.desc = &s_desc;
    plug->plugin.plugin_data = plug;
    plug->plugin.init = go_init;
    plug->plugin.destroy = go_destroy;
    plug->plugin.activate = go_activate;
    plug->plugin.deactivate = go_deactivate;
    plug->plugin.start_processing = go_start_processing;
    plug->plugin.stop_processing = go_stop_processing;
    plug->plugin.reset = go_reset;
    plug->plugin.process = go_process;
    plug->plugin.get_extension = go_get_extension;
    plug->plugin.on_main_thread = go_on_main_thread;
    return &plug->plugin;
}

static uint32_t factory_count(const clap_plugin_factory_t *f)
{
    (void)f;
    return 1;
}

static const clap_plugin_descriptor_t *factory_desc(const clap_plugin_factory_t *f,
                                                    uint32_t index)
{
    (void)f;
    if (index != 0) return NULL;
    return &s_desc;
}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *f,
                                           const clap_host_t *host,
                                           const char *plugin_id)
{
    (void)f;
    if (!host || !plugin_id) return NULL;
    if (strcmp(plugin_id, s_desc.id) != 0) return NULL;
    if (host->clap_version.major < 1) return NULL;
    return go_create(host);
}

static const clap_plugin_factory_t s_factory = {
    .get_plugin_count      = factory_count,
    .get_plugin_descriptor = factory_desc,
    .create_plugin         = factory_create
};

static bool entry_init(const char *plugin_path)
{
    (void)plugin_path;
    return true;
}

static void entry_deinit(void) {}

static const void *entry_get_factory(const char *factory_id)
{
    if (!strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID)) return &s_factory;
    return NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init         = entry_init,
    .deinit       = entry_deinit,
    .get_factory  = entry_get_factory
};
