/*
 * Virtual HDMI to DVI converter
 *
 * Copyright (C) 2020 Elesar Ltd. - http://www.elesar.co.uk/
 * Author: James Byrne <jbyrne@elesar.co.uk>
 *
 * Based on encoder-tpd12s015.c
 * Copyright (C) 2013 Texas Instruments Incorporated
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/platform_device.h>

#include "../dss/omapdss.h"

struct drv_data {
	struct omap_dss_device dssdev;
	struct omap_dss_device *in;
	struct videomode vm;
};

#define to_drv_data(x) container_of(x, struct drv_data, dssdev)

static int htod_connect(struct omap_dss_device *dssdev,
		struct omap_dss_device *dst)
{
	struct drv_data *ddata = to_drv_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	int r;

	r = in->ops.hdmi->connect(in, dssdev);
	if (r)
		return r;

	dst->src = dssdev;
	dssdev->dst = dst;

	return 0;
}

static void htod_disconnect(struct omap_dss_device *dssdev,
		struct omap_dss_device *dst)
{
	struct drv_data *ddata = to_drv_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	WARN_ON(dst != dssdev->dst);

	if (dst != dssdev->dst)
		return;

	dst->src = NULL;
	dssdev->dst = NULL;

	in->ops.hdmi->disconnect(in, &ddata->dssdev);
}

static int htod_enable(struct omap_dss_device *dssdev)
{
	struct drv_data *ddata = to_drv_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	int r;

	if (dssdev->state == OMAP_DSS_DISPLAY_ACTIVE)
		return 0;

	in->ops.hdmi->set_timings(in, &ddata->vm);

	r = in->ops.hdmi->enable(in);
	if (r)
		return r;

	dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;

	return r;
}

static void htod_disable(struct omap_dss_device *dssdev)
{
	struct drv_data *ddata = to_drv_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	if (dssdev->state != OMAP_DSS_DISPLAY_ACTIVE)
		return;

	in->ops.hdmi->disable(in);

	dssdev->state = OMAP_DSS_DISPLAY_DISABLED;
}

static void htod_set_timings(struct omap_dss_device *dssdev,
			    struct videomode *vm)
{
	struct drv_data *ddata = to_drv_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	ddata->vm = *vm;
	dssdev->panel.vm = *vm;

	in->ops.hdmi->set_timings(in, vm);
}

static void htod_get_timings(struct omap_dss_device *dssdev,
			    struct videomode *vm)
{
	struct drv_data *ddata = to_drv_data(dssdev);

	*vm = ddata->vm;
}

static int htod_check_timings(struct omap_dss_device *dssdev,
			     struct videomode *vm)
{
	struct drv_data *ddata = to_drv_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	int r;

	r = in->ops.hdmi->check_timings(in, vm);

	return r;
}

static const struct omapdss_dvi_ops htod_dvi_ops = {
	.connect		= htod_connect,
	.disconnect		= htod_disconnect,

	.enable			= htod_enable,
	.disable		= htod_disable,

	.check_timings		= htod_check_timings,
	.set_timings		= htod_set_timings,
	.get_timings		= htod_get_timings,
};

static int htod_probe_of(struct platform_device *pdev)
{
	struct drv_data *ddata = platform_get_drvdata(pdev);
	struct device_node *node = pdev->dev.of_node;
	struct omap_dss_device *in;

	in = omapdss_of_find_source_for_first_ep(node);
	if (IS_ERR(in)) {
		dev_err(&pdev->dev, "failed to find video source\n");
		return PTR_ERR(in);
	}

	ddata->in = in;

	return 0;
}

static int htod_probe(struct platform_device *pdev)
{
	struct omap_dss_device *in, *dssdev;
	struct drv_data *ddata;
	int r;

	ddata = devm_kzalloc(&pdev->dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	platform_set_drvdata(pdev, ddata);

	if (!pdev->dev.of_node)
		return -ENODEV;

	r = htod_probe_of(pdev);
	if (r)
		return r;

	dssdev = &ddata->dssdev;
	dssdev->ops.dvi = &htod_dvi_ops;
	dssdev->dev = &pdev->dev;
	dssdev->type = OMAP_DISPLAY_TYPE_HDMI;
	dssdev->output_type = OMAP_DISPLAY_TYPE_DVI;
	dssdev->owner = THIS_MODULE;
	dssdev->port_num = 1;

	in = ddata->in;

	r = omapdss_register_output(dssdev);
	if (r) {
		dev_err(&pdev->dev, "Failed to register output\n");
		goto err_reg;
	}

	return 0;
err_reg:
	omap_dss_put_device(ddata->in);
	return r;
}

static int __exit htod_remove(struct platform_device *pdev)
{
	struct drv_data *ddata = platform_get_drvdata(pdev);
	struct omap_dss_device *dssdev = &ddata->dssdev;
	struct omap_dss_device *in = ddata->in;

	omapdss_unregister_output(&ddata->dssdev);

	WARN_ON(omapdss_device_is_enabled(dssdev));
	if (omapdss_device_is_enabled(dssdev))
		htod_disable(dssdev);

	WARN_ON(omapdss_device_is_connected(dssdev));
	if (omapdss_device_is_connected(dssdev))
		htod_disconnect(dssdev, dssdev->dst);

	omap_dss_put_device(in);

	return 0;
}

static const struct of_device_id htod_of_match[] = {
	{ .compatible = "omapdss,ti,hdmitodvi", },
	{},
};

MODULE_DEVICE_TABLE(of, htod_of_match);

static struct platform_driver htod_driver = {
	.probe	= htod_probe,
	.remove	= __exit_p(htod_remove),
	.driver	= {
		.name	= "hdmitodvi",
		.of_match_table = htod_of_match,
		.suppress_bind_attrs = true,
	},
};

module_platform_driver(htod_driver);

MODULE_AUTHOR("James Byrne <jbyrne@elesar.co.uk>");
MODULE_DESCRIPTION("HDMI to DVI driver");
MODULE_LICENSE("GPL");
