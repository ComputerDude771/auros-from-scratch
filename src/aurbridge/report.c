/* Rendering a preflight report.
 *
 * Two audiences: a person in a terminal, and the wizard UI (JSON). The
 * human form deliberately leads with what is wrong and what to do about
 * it, because a refusal the user cannot act on is just a dead end. */
#include "preflight.h"
#include <stdio.h>
#include <string.h>

static const char *sev_tag(pf_severity s)
{
    switch (s) {
        case PF_BLOCK: return "STOP";
        case PF_WARN:  return "WARN";
        case PF_INFO:  return "INFO";
        default:       return " OK ";
    }
}
static const char *sev_color(pf_severity s)
{
    switch (s) {
        case PF_BLOCK: return "\033[38;2;242;120;141m";
        case PF_WARN:  return "\033[38;2;242;184;128m";
        case PF_INFO:  return "\033[38;2;130;170;255m";
        default:       return "\033[38;2;125;211;192m";
    }
}
#define RST "\033[0m"
#define DIM "\033[38;2;110;120;134m"

static void human_size(uint64_t b, char *out, size_t n)
{
    const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)b; int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    snprintf(out, n, "%.1f %s", v, u[i]);
}

void pf_print_human(const pf_report *r)
{
    char buf[64];
    printf("\n" DIM "  AurBridge preflight" RST "\n\n");

    printf("  %-22s %s\n", "Firmware",
           r->is_uefi == 1 ? "UEFI" : r->is_uefi == 0 ? "Legacy BIOS" : "unknown");
    printf("  %-22s %s\n", "Secure Boot",
           r->secure_boot == 1 ? "on" : r->secure_boot == 0 ? "off" : "unknown");
    printf("  %-22s %s", "Power", r->on_ac_power == 1 ? "plugged in" : "on battery");
    if (r->battery_percent >= 0) printf(" (%d%%)", r->battery_percent);
    printf("\n");
    human_size(r->ram_bytes, buf, sizeof buf);
    printf("  %-22s %s\n", "Memory", buf);

    for (int i = 0; i < r->n_disks; i++) {
        const pf_disk *d = &r->disks[i];
        human_size(d->size_bytes, buf, sizeof buf);
        printf("  %-22s %s  %s%s%s\n",
               i == 0 ? "Disks" : "",
               buf, d->model[0] ? d->model : "(unknown)",
               d->is_system ? "  [Windows]" : "",
               d->is_removable ? "  [removable]" : "");
    }
    for (int i = 0; i < r->n_volumes; i++) {
        const pf_volume *v = &r->volumes[i];
        char fb[64];
        human_size(v->size_bytes, buf, sizeof buf);
        human_size(v->free_bytes, fb, sizeof fb);
        printf("  %-22s %s  %s, %s free%s\n",
               i == 0 ? "Volumes" : "", v->mount, buf, fb,
               v->bitlocker == 1 ? "  [BitLocker]" : "");
    }

    printf("\n");
    /* Blocks first: they are the only thing that decides the outcome. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < r->n; i++) {
            const pf_result *x = &r->results[i];
            int is_block = (x->sev == PF_BLOCK);
            if (pass == 0 && !is_block) continue;
            if (pass == 1 && is_block) continue;
            if (pass == 1 && x->sev == PF_PASS && r->n_block) continue;

            printf("  %s[%s]%s %s", sev_color(x->sev), sev_tag(x->sev), RST, x->title);
            if (x->risk[0]) printf(DIM "  (%s)" RST, x->risk);
            printf("\n");
            if (x->detail[0]) printf(DIM "         %s" RST "\n", x->detail);
            if (x->remedy[0]) printf("         %s\n", x->remedy);
            printf("\n");
        }
    }

    if (r->n_block) {
        printf("  %sAurBridge will not continue.%s  %d blocking issue%s",
               sev_color(PF_BLOCK), RST, r->n_block, r->n_block == 1 ? "" : "s");
        if (r->n_warn) printf(", %d warning%s", r->n_warn, r->n_warn == 1 ? "" : "s");
        printf(".\n" DIM "  Nothing on this PC has been changed." RST "\n\n");
    } else {
        printf("  %sReady to install.%s", sev_color(PF_PASS), RST);
        if (r->n_warn) printf("  %d warning%s to review first.",
                              r->n_warn, r->n_warn == 1 ? "" : "s");
        printf("\n\n");
    }
}

static void json_escape(const char *s, char *out, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; s[i] && j + 2 < n; i++) {
        if (s[i] == '"' || s[i] == '\\') { out[j++] = '\\'; out[j++] = s[i]; }
        else if ((unsigned char)s[i] < 0x20) { if (j + 6 < n) j += snprintf(out+j, n-j, "\\u%04x", s[i]); }
        else out[j++] = s[i];
    }
    out[j] = '\0';
}

void pf_print_json(const pf_report *r)
{
    char e1[1024], e2[1024], e3[1024];
    printf("{\n");
    printf("  \"go\": %s,\n", pf_is_go(r) ? "true" : "false");
    printf("  \"blocks\": %d, \"warnings\": %d,\n", r->n_block, r->n_warn);
    printf("  \"machine\": {\n");
    printf("    \"uefi\": %s, \"secure_boot\": %d, \"admin\": %s,\n",
           r->is_uefi == 1 ? "true" : "false", r->secure_boot, r->is_admin ? "true" : "false");
    printf("    \"on_ac\": %s, \"battery_percent\": %d, \"ram_bytes\": %llu\n",
           r->on_ac_power == 1 ? "true" : "false", r->battery_percent,
           (unsigned long long)r->ram_bytes);
    printf("  },\n");

    printf("  \"disks\": [\n");
    for (int i = 0; i < r->n_disks; i++) {
        const pf_disk *d = &r->disks[i];
        json_escape(d->model, e1, sizeof e1);
        printf("    {\"index\": %d, \"model\": \"%s\", \"size\": %llu, \"system\": %s, "
               "\"removable\": %s, \"style\": \"%s\", \"smart_ok\": %d, "
               "\"reallocated\": %lu, \"pending\": %lu, \"uncorrectable\": %lu}%s\n",
               d->index, e1, (unsigned long long)d->size_bytes,
               d->is_system ? "true" : "false", d->is_removable ? "true" : "false",
               d->partition_style == 1 ? "GPT" : d->partition_style == 0 ? "MBR" : "RAW",
               d->smart_ok, (unsigned long)d->smart_reallocated,
               (unsigned long)d->smart_pending, (unsigned long)d->smart_uncorrectable,
               i + 1 < r->n_disks ? "," : "");
    }
    printf("  ],\n");

    printf("  \"volumes\": [\n");
    for (int i = 0; i < r->n_volumes; i++) {
        const pf_volume *v = &r->volumes[i];
        printf("    {\"mount\": \"%s\", \"fs\": \"%s\", \"size\": %llu, \"free\": %llu, "
               "\"bitlocker\": %d, \"dirty\": %d}%s\n",
               v->mount, v->fs, (unsigned long long)v->size_bytes,
               (unsigned long long)v->free_bytes, v->bitlocker, v->dirty,
               i + 1 < r->n_volumes ? "," : "");
    }
    printf("  ],\n");

    printf("  \"findings\": [\n");
    for (int i = 0; i < r->n; i++) {
        const pf_result *x = &r->results[i];
        json_escape(x->title,  e1, sizeof e1);
        json_escape(x->detail, e2, sizeof e2);
        json_escape(x->remedy, e3, sizeof e3);
        printf("    {\"id\": \"%s\", \"risk\": \"%s\", \"severity\": \"%s\", "
               "\"title\": \"%s\", \"detail\": \"%s\", \"remedy\": \"%s\"}%s\n",
               x->id, x->risk,
               x->sev == PF_BLOCK ? "block" : x->sev == PF_WARN ? "warn" :
               x->sev == PF_INFO ? "info" : "pass",
               e1, e2, e3, i + 1 < r->n ? "," : "");
    }
    printf("  ]\n}\n");
}
