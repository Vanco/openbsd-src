/* $OpenBSD: usty.c,v 1.0 2026/09/14 Exp $ */
/*
 * USB HID touchscreen and stylus driver for Huawei MateBook E 2022
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/device.h>
#include <sys/ioctl.h>
#include <sys/malloc.h>

#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wsmousevar.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbhid.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/uhidev.h>

#include <dev/hid/hid.h>


struct usty_softc {
    struct uhidev       sc_hdev;
    struct device      *sc_wsmousedev;

    int                 sc_reportid;      /* report ID */
    int                 sc_is_pen;     /* is pen */

    struct hid_location sc_xloc, sc_yloc;
    struct hid_location sc_tiploc, sc_pressureloc;

    struct hid_location sc_inrangeloc;

    int                 sc_minx, sc_maxx;
    int                 sc_miny, sc_maxy;
    int                 sc_isize;

    int                 sc_enabled;
    int                 sc_buttons;
};

int  usty_match(struct device *, void *, void *);
void usty_attach(struct device *, struct device *, void *);
int  usty_detach(struct device *, int);

int  usty_enable(void *);
void usty_disable(void *);
int  usty_ioctl(void *, u_long, caddr_t, int, struct proc *);
void usty_intr(struct uhidev *, void *, u_int);

const struct wsmouse_accessops usty_accessops = {
    usty_enable,
    usty_ioctl,
    usty_disable,
};

struct cfdriver usty_cd = {
    NULL, "usty", DV_DULL
};

const struct cfattach usty_ca = {
    sizeof(struct usty_softc),
    usty_match,
    usty_attach,
    usty_detach
};

/* ---- match ---- */

int
usty_match(struct device *parent, void *match, void *aux)
{
    struct uhidev_attach_arg *uha = (struct uhidev_attach_arg *)aux;
    int size;
    void *desc;
    int r_ts, r_pen;

    printf("usty_match: reportid=%d\n", uha->reportid);

    if (UHIDEV_CLAIM_MULTIPLE_REPORTID(uha))
        return UMATCH_NONE;

    uhidev_get_report_desc(uha->parent, &desc, &size);

    if (hid_report_size(desc, size, hid_input, uha->reportid) == 0)
        return UMATCH_NONE;

    r_ts = hid_is_collection(desc, size, uha->reportid,
        HID_USAGE2(HUP_DIGITIZERS, HUD_TOUCHSCREEN));
    r_pen = hid_is_collection(desc, size, uha->reportid,
        HID_USAGE2(HUP_DIGITIZERS, HUD_PEN));

    printf("usty_match: rid=%d ts=%d pen=%d\n",
        uha->reportid, r_ts, r_pen);

    if (r_ts || r_pen)
        return UMATCH_IFACECLASS;

    return UMATCH_NONE;
    
}

/* ---- attach ---- */

void
usty_attach(struct device *parent, struct device *self, void *aux)
{
    struct usty_softc *sc = (struct usty_softc *)self;
    struct uhidev_attach_arg *uha = (struct uhidev_attach_arg *)aux;
    struct usb_attach_arg *uaa = uha->uaa;
    int size;
    void *desc;

    sc->sc_hdev.sc_intr = usty_intr;
    sc->sc_hdev.sc_parent = uha->parent;
    sc->sc_hdev.sc_udev = uaa->device;
    sc->sc_hdev.sc_report_id = uha->reportid;   /* 绑定到本 report ID */
    sc->sc_reportid = uha->reportid;

    usbd_set_idle(uha->parent->sc_udev, uha->parent->sc_ifaceno, 0, 0);

    uhidev_get_report_desc(uha->parent, &desc, &size);

    if (hid_is_collection(desc, size, uha->reportid,
        HID_USAGE2(HUP_DIGITIZERS, HUD_TOUCHSCREEN))) {
        sc->sc_is_pen = 0;
        printf("usty_attach: touchscreen reportid %d\n", uha->reportid);
    } else {
        sc->sc_is_pen = 1;
        printf("usty_attach: pen reportid %d\n", uha->reportid);
    }

    sc->sc_hdev.sc_isize = hid_report_size(desc, size, hid_input, uha->reportid);

    hid_locate(desc, size, HID_USAGE2(HUP_GENERIC_DESKTOP, 0x0030),
        uha->reportid, hid_input, &sc->sc_xloc, NULL);
    hid_locate(desc, size, HID_USAGE2(HUP_GENERIC_DESKTOP, 0x0031),
        uha->reportid, hid_input, &sc->sc_yloc, NULL);
    hid_locate(desc, size, HID_USAGE2(HUP_DIGITIZERS, HUD_TIP_SWITCH),
        uha->reportid, hid_input, &sc->sc_tiploc, NULL);
    hid_locate(desc, size, HID_USAGE2(HUP_DIGITIZERS, HUD_TIP_PRESSURE),
        uha->reportid, hid_input, &sc->sc_pressureloc, NULL);

    if (sc->sc_is_pen)
        hid_locate(desc, size, HID_USAGE2(HUP_DIGITIZERS, HUD_IN_RANGE),
        uha->reportid, hid_input, &sc->sc_inrangeloc, NULL);
    
    struct hid_data *hd;
    struct hid_item h;

    hd = hid_start_parse(desc, size, hid_input);
    while (hid_get_item(hd, &h)) {
        if (h.kind != hid_input)
            continue;
        if (h.report_ID == sc->sc_reportid) {
            switch (h.usage) {
            case HID_USAGE2(HUP_GENERIC_DESKTOP, 0x0030):  /* X */
                if (h.logical_maximum > h.logical_minimum) {
                    sc->sc_minx = h.logical_minimum;
                    sc->sc_maxx = h.logical_maximum;
                }
                break;
            case HID_USAGE2(HUP_GENERIC_DESKTOP, 0x0031):  /* Y */
                if (h.logical_maximum > h.logical_minimum) {
                    sc->sc_miny = h.logical_minimum;
                    sc->sc_maxy = h.logical_maximum;
                }
                break;
            }
        }
    }
    hid_end_parse(hd);
    
    printf(" : %s rid %d\n", sc->sc_is_pen ? "pen" : "touchscreen",
        uha->reportid);

    /* attach wsmouse */
    {
        struct wsmousedev_attach_args a;
        struct wsmousehw *hw;
        
        a.accessops = &usty_accessops;
        a.accesscookie = sc;
        sc->sc_wsmousedev = config_found(self, &a, wsmousedevprint);

        hw = wsmouse_get_hw(sc->sc_wsmousedev);
        hw->type = WSMOUSE_TYPE_TPANEL;
        hw->x_min = sc->sc_minx;
        hw->x_max = sc->sc_maxx;
        hw->y_min = sc->sc_miny;
        hw->y_max = sc->sc_maxy;

        printf("usty_attach: hw type=%d hw_type=%d mt_slots=%d\n",
            hw->type, hw->hw_type, hw->mt_slots);

        wsmouse_configure(sc->sc_wsmousedev, NULL, 0);
    }

    printf(" %s : ts range %dx%d\n", self->dv_xname,
        sc->sc_maxx, sc->sc_maxy);
}

int
usty_detach(struct device *self, int flags)
{
    struct usty_softc *sc = (struct usty_softc *)self;
    int rv = 0;
    if (sc->sc_wsmousedev != NULL)
        rv = config_detach(sc->sc_wsmousedev, flags);
    return rv;
}

/* ---- enable / disable ---- */

int
usty_enable(void *v)
{
    struct usty_softc *sc = v;
    int rv;

    printf("usty_enable called\n");

    if (sc->sc_enabled)
        return EBUSY;

    rv = uhidev_open(&sc->sc_hdev);
    printf("usty_enable: uhidev_open ret=%d\n", rv);
    if (rv != 0)
        return rv;

    sc->sc_enabled = 1;
    return 0;
}

void
usty_disable(void *v)
{
    struct usty_softc *sc = v;
    if (!sc->sc_enabled)
        return;
    uhidev_close(&sc->sc_hdev);
    sc->sc_enabled = 0;
}

/* ---- ioctl ---- */

int
usty_ioctl(void *v, u_long cmd, caddr_t data, int flag, struct proc *p)
{
    struct usty_softc *sc = v;
    int rc;

    rc = uhidev_ioctl(&sc->sc_hdev, cmd, data, flag, p);
    if (rc != -1)
        return rc;

    switch (cmd) {
    case WSMOUSEIO_GTYPE:
        *(u_int *)data = WSMOUSE_TYPE_TPANEL;
        return 0;

    case WSMOUSEIO_GCALIBCOORDS: {
        struct wsmouse_calibcoords *wsmc =
            (struct wsmouse_calibcoords *)data;
        wsmc->minx = 0;
        wsmc->maxx = sc->sc_maxx > 0 ? sc->sc_maxx : 4095;
        wsmc->miny = 0;
        wsmc->maxy = sc->sc_maxy > 0 ? sc->sc_maxy : 4095;
        wsmc->swapxy = 0;
        wsmc->resx = 0;
        wsmc->resy = 0;
        return 0;
    }
    }
    return -1;
}

/* ---- interrupt handler ---- */

void
usty_intr(struct uhidev *dev, void *buf, u_int len)
{
    struct usty_softc *sc = (struct usty_softc *)dev;
    uint8_t *data = buf;
    int x, y, in_range, tip, pressure;
    int s;

    //printf("usty_intr: len=%u data0=0x%02x enabled=%d\n",
    //    len, len > 0 ? data[0] : 0, sc->sc_enabled);

    if (!sc->sc_enabled)
        return;

    x = hid_get_udata(data, len, &sc->sc_xloc);
    y = hid_get_udata(data, len, &sc->sc_yloc);
    tip = hid_get_udata(data, len, &sc->sc_tiploc);
    in_range = hid_get_udata(data, len, &sc->sc_inrangeloc);
    pressure = hid_get_udata(data, len, &sc->sc_pressureloc);

    s = spltty();
    wsmouse_position(sc->sc_wsmousedev, x, y);
    wsmouse_buttons(sc->sc_wsmousedev, tip ? 1 : 0);
    if (in_range) {
        if (tip) {
            // 笔尖接触，上报实际压力
            wsmouse_mtstate(sc->sc_wsmousedev, 0, x, y, pressure);
        } else {
            // 笔悬停，使用默认压力表示“存在但未接触”
            wsmouse_mtstate(sc->sc_wsmousedev, 0, x, y, WSMOUSE_DEFAULT_PRESSURE);
        }
    } else {
        // 笔离开范围，上报压力0表示触点结束
        wsmouse_mtstate(sc->sc_wsmousedev, 0, x, y, 0);
    }
    wsmouse_input_sync(sc->sc_wsmousedev);
    splx(s);

}
