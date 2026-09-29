/*
 * Protocol tables for kde_output_management_v2 and kde_output_device_v2
 * (plasma-wayland-protocols kde-output-management-v2.xml and
 * kde-output-device-v2.xml) up to version 21: what wayland-scanner would
 * generate. Unused requests keep their slot but no argument types.
 *
 * Shared by output.c and the host test's stand-in for KWin. The includer
 * provides struct wl_interface / wl_message and an all-NULL k_null.
 */
#ifndef CHAM_OUTPUT_PROTO_H
#define CHAM_OUTPUT_PROTO_H

static const struct wl_interface k_od_registry_iface;
static const struct wl_interface k_od_iface;
static const struct wl_interface k_od_mode_iface;
static const struct wl_interface k_om_iface;
static const struct wl_interface k_oc_iface;
static const struct wl_interface k_mode_list_iface;

static const struct wl_interface *k_od_registry_output_types[] = {&k_od_iface};
static const struct wl_message k_od_registry_requests[] = {
    {"stop", "21", k_null}, /* 0 */
};
static const struct wl_message k_od_registry_events[] = {
    {"finished", "21", k_null}, /* 0 */
    {"output", "21n", k_od_registry_output_types}, /* 1 */
};
static const struct wl_interface k_od_registry_iface = {
    "kde_output_device_registry_v2", 21, 1, k_od_registry_requests, 2, k_od_registry_events,
};
static const struct wl_interface *k_od_current_mode_types[] = {&k_od_mode_iface};
static const struct wl_interface *k_od_mode_types[] = {&k_od_mode_iface};
static const struct wl_message k_od_requests[] = {
    {"release", "21", k_null}, /* 0 */
};
static const struct wl_message k_od_events[] = {
    {"geometry", "iiiiissi", k_null}, /* 0 */
    {"current_mode", "o", k_od_current_mode_types}, /* 1 */
    {"mode", "n", k_od_mode_types}, /* 2 */
    {"done", "", k_null}, /* 3 */
    {"scale", "f", k_null}, /* 4 */
    {"edid", "s", k_null}, /* 5 */
    {"enabled", "i", k_null}, /* 6 */
    {"uuid", "s", k_null}, /* 7 */
    {"serial_number", "s", k_null}, /* 8 */
    {"eisa_id", "s", k_null}, /* 9 */
    {"capabilities", "u", k_null}, /* 10 */
    {"overscan", "u", k_null}, /* 11 */
    {"vrr_policy", "u", k_null}, /* 12 */
    {"rgb_range", "u", k_null}, /* 13 */
    {"name", "2s", k_null}, /* 14 */
    {"high_dynamic_range", "3u", k_null}, /* 15 */
    {"sdr_brightness", "3u", k_null}, /* 16 */
    {"wide_color_gamut", "3u", k_null}, /* 17 */
    {"auto_rotate_policy", "4u", k_null}, /* 18 */
    {"icc_profile_path", "5s", k_null}, /* 19 */
    {"brightness_metadata", "6uuu", k_null}, /* 20 */
    {"brightness_overrides", "6iii", k_null}, /* 21 */
    {"sdr_gamut_wideness", "6u", k_null}, /* 22 */
    {"color_profile_source", "7u", k_null}, /* 23 */
    {"brightness", "8u", k_null}, /* 24 */
    {"color_power_tradeoff", "10u", k_null}, /* 25 */
    {"dimming", "11u", k_null}, /* 26 */
    {"replication_source", "13s", k_null}, /* 27 */
    {"ddc_ci_allowed", "14u", k_null}, /* 28 */
    {"max_bits_per_color", "15u", k_null}, /* 29 */
    {"max_bits_per_color_range", "15uu", k_null}, /* 30 */
    {"automatic_max_bits_per_color_limit", "15u", k_null}, /* 31 */
    {"edr_policy", "16u", k_null}, /* 32 */
    {"sharpness", "17u", k_null}, /* 33 */
    {"priority", "18u", k_null}, /* 34 */
    {"auto_brightness", "20u", k_null}, /* 35 */
    {"removed", "21", k_null}, /* 36 */
};
static const struct wl_interface k_od_iface = {
    "kde_output_device_v2", 21, 1, k_od_requests, 37, k_od_events,
};
static const struct wl_message k_od_mode_events[] = {
    {"size", "ii", k_null}, /* 0 */
    {"refresh", "i", k_null}, /* 1 */
    {"preferred", "", k_null}, /* 2 */
    {"removed", "", k_null}, /* 3 */
    {"flags", "19u", k_null}, /* 4 */
};
static const struct wl_interface k_od_mode_iface = {
    "kde_output_device_mode_v2", 21, 0, NULL, 5, k_od_mode_events,
};
static const struct wl_interface *k_om_create_configuration_types[] = {&k_oc_iface};
static const struct wl_interface *k_om_create_mode_list_types[] = {&k_mode_list_iface};
static const struct wl_message k_om_requests[] = {
    {"create_configuration", "n", k_om_create_configuration_types}, /* 0 */
    {"create_mode_list", "n", k_om_create_mode_list_types}, /* 1 */
};
static const struct wl_interface k_om_iface = {
    "kde_output_management_v2", 21, 2, k_om_requests, 0, NULL,
};
static const struct wl_interface *k_oc_mode_types[] = {&k_od_iface, &k_od_mode_iface};
static const struct wl_interface *k_oc_set_custom_modes_types[] = {&k_od_iface, &k_mode_list_iface};
static const struct wl_message k_oc_requests[] = {
    {"enable", "oi", k_null}, /* 0 */
    {"mode", "oo", k_oc_mode_types}, /* 1 */
    {"transform", "oi", k_null}, /* 2 */
    {"position", "oii", k_null}, /* 3 */
    {"scale", "of", k_null}, /* 4 */
    {"apply", "", k_null}, /* 5 */
    {"destroy", "", k_null}, /* 6 */
    {"overscan", "ou", k_null}, /* 7 */
    {"set_vrr_policy", "ou", k_null}, /* 8 */
    {"set_rgb_range", "ou", k_null}, /* 9 */
    {"set_primary_output", "2o", k_null}, /* 10 */
    {"set_priority", "3ou", k_null}, /* 11 */
    {"set_high_dynamic_range", "4ou", k_null}, /* 12 */
    {"set_sdr_brightness", "4ou", k_null}, /* 13 */
    {"set_wide_color_gamut", "4ou", k_null}, /* 14 */
    {"set_auto_rotate_policy", "5ou", k_null}, /* 15 */
    {"set_icc_profile_path", "6os", k_null}, /* 16 */
    {"set_brightness_overrides", "7oiii", k_null}, /* 17 */
    {"set_sdr_gamut_wideness", "7ou", k_null}, /* 18 */
    {"set_color_profile_source", "8ou", k_null}, /* 19 */
    {"set_brightness", "9ou", k_null}, /* 20 */
    {"set_color_power_tradeoff", "10ou", k_null}, /* 21 */
    {"set_dimming", "11ou", k_null}, /* 22 */
    {"set_replication_source", "13os", k_null}, /* 23 */
    {"set_ddc_ci_allowed", "14ou", k_null}, /* 24 */
    {"set_max_bits_per_color", "15ou", k_null}, /* 25 */
    {"set_edr_policy", "16ou", k_null}, /* 26 */
    {"set_sharpness", "17ou", k_null}, /* 27 */
    {"set_custom_modes", "18oo", k_oc_set_custom_modes_types}, /* 28 */
    {"set_auto_brightness", "19ou", k_null}, /* 29 */
    {"set_hdr_icc_profile_path", "20os", k_null}, /* 30 */
    {"set_hdr_color_profile_source", "20ou", k_null}, /* 31 */
    {"set_abm_level", "21ou", k_null}, /* 32 */
};
static const struct wl_message k_oc_events[] = {
    {"applied", "", k_null}, /* 0 */
    {"failed", "", k_null}, /* 1 */
    {"failure_reason", "12s", k_null}, /* 2 */
};
static const struct wl_interface k_oc_iface = {
    "kde_output_configuration_v2", 21, 33, k_oc_requests, 3, k_oc_events,
};
static const struct wl_message k_mode_list_requests[] = {
    {"destroy", "", k_null}, /* 0 */
    {"add_mode", "", k_null}, /* 1 */
    {"set_resolution", "uu", k_null}, /* 2 */
    {"set_refresh_rate", "u", k_null}, /* 3 */
    {"set_reduced_blanking", "u", k_null}, /* 4 */
};
static const struct wl_interface k_mode_list_iface = {
    "kde_mode_list_v2", 21, 5, k_mode_list_requests, 0, NULL,
};

enum { OD_REGISTRY_EV_FINISHED, OD_REGISTRY_EV_OUTPUT };
enum {
    OD_EV_CURRENT_MODE = 1, OD_EV_MODE = 2, OD_EV_DONE = 3, OD_EV_CAPABILITIES = 10, OD_EV_REMOVED = 36,
    OD_EV_COUNT = 37,
};
enum { OD_REQ_RELEASE = 0 };
enum { MODE_EV_SIZE, MODE_EV_REFRESH, MODE_EV_PREFERRED, MODE_EV_REMOVED, MODE_EV_FLAGS };
enum { OM_REQ_CREATE_CONFIGURATION, OM_REQ_CREATE_MODE_LIST };
enum { OC_REQ_MODE = 1, OC_REQ_APPLY = 5, OC_REQ_DESTROY = 6, OC_REQ_SET_CUSTOM_MODES = 28, OC_REQ_COUNT = 33 };
enum { OC_EV_APPLIED, OC_EV_FAILED, OC_EV_FAILURE_REASON };
enum { ML_REQ_DESTROY, ML_REQ_ADD_MODE, ML_REQ_SET_RESOLUTION, ML_REQ_SET_REFRESH_RATE, ML_REQ_SET_REDUCED_BLANKING };
#define OD_CAPABILITY_CUSTOM_MODES 0x2000u
#define OUTPUT_PROTO_VERSION 21u
#define OC_SET_CUSTOM_MODES_SINCE 18u

#endif
