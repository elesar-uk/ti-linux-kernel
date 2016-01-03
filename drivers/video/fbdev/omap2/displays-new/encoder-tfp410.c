/*
 * TFP410 DPI-to-DVI encoder driver
 *
 * Copyright (C) 2013 Texas Instruments
 * Author: Tomi Valkeinen <tomi.valkeinen@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/of_gpio.h>
#include <linux/i2c.h>
#include <linux/regmap.h>

#include <video/omapdss.h>
#include <video/omap-panel-data.h>

#define TFP410_I2C_NAME "tfp410"

/* I2C registers */
#define TFP410_VEN_ID_L		0x00
#define TFP410_VEN_ID_H		0x01
#define TFP410_DEV_ID_L		0x02
#define TFP410_DEV_ID_H		0x03
#define TFP410_REV_ID		0x04
#define TFP410_CTL_1_MODE	0x08
#define TFP410_CTL_2_MODE	0x09
#define TFP410_CTL_3_MODE	0x0A
#define TFP410_CFG		0x0B

struct panel_drv_data {
	struct omap_dss_device dssdev;
	struct omap_dss_device *in;

	int pd_gpio;
	int data_lines;

	struct omap_video_timings timings;
};

#define to_panel_data(x) container_of(x, struct panel_drv_data, dssdev)

static int tfp410_connect(struct omap_dss_device *dssdev,
		struct omap_dss_device *dst)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	int r;

	if (omapdss_device_is_connected(dssdev))
		return -EBUSY;

	r = in->ops.dpi->connect(in, dssdev);
	if (r)
		return r;

	dst->src = dssdev;
	dssdev->dst = dst;

	return 0;
}

static void tfp410_disconnect(struct omap_dss_device *dssdev,
		struct omap_dss_device *dst)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	WARN_ON(!omapdss_device_is_connected(dssdev));
	if (!omapdss_device_is_connected(dssdev))
		return;

	WARN_ON(dst != dssdev->dst);
	if (dst != dssdev->dst)
		return;

	dst->src = NULL;
	dssdev->dst = NULL;

	in->ops.dpi->disconnect(in, &ddata->dssdev);
}

static int tfp410_enable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	int r;

	if (!omapdss_device_is_connected(dssdev))
		return -ENODEV;

	if (omapdss_device_is_enabled(dssdev))
		return 0;

	in->ops.dpi->set_timings(in, &ddata->timings);
	if (ddata->data_lines)
		in->ops.dpi->set_data_lines(in, ddata->data_lines);

	r = in->ops.dpi->enable(in);
	if (r)
		return r;

	if (gpio_is_valid(ddata->pd_gpio))
		gpio_set_value_cansleep(ddata->pd_gpio, 1);

	dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;

	return 0;
}

static void tfp410_disable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	if (!omapdss_device_is_enabled(dssdev))
		return;

	if (gpio_is_valid(ddata->pd_gpio))
		gpio_set_value_cansleep(ddata->pd_gpio, 0);

	in->ops.dpi->disable(in);

	dssdev->state = OMAP_DSS_DISPLAY_DISABLED;
}

static void tfp410_fix_timings(struct omap_video_timings *timings)
{
	timings->data_pclk_edge = OMAPDSS_DRIVE_SIG_RISING_EDGE;
	timings->sync_pclk_edge = OMAPDSS_DRIVE_SIG_RISING_EDGE;
	timings->de_level = OMAPDSS_SIG_ACTIVE_HIGH;
}

static void tfp410_set_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	tfp410_fix_timings(timings);

	ddata->timings = *timings;
	dssdev->panel.timings = *timings;

	in->ops.dpi->set_timings(in, timings);
}

static void tfp410_get_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);

	*timings = ddata->timings;
}

static int tfp410_check_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	tfp410_fix_timings(timings);

	return in->ops.dpi->check_timings(in, timings);
}

static const struct omapdss_dvi_ops tfp410_dvi_ops = {
	.connect	= tfp410_connect,
	.disconnect	= tfp410_disconnect,

	.enable		= tfp410_enable,
	.disable	= tfp410_disable,

	.check_timings	= tfp410_check_timings,
	.set_timings	= tfp410_set_timings,
	.get_timings	= tfp410_get_timings,
};

static int tfp410_probe_pdata(struct platform_device *pdev)
{
	struct panel_drv_data *ddata = platform_get_drvdata(pdev);
	struct encoder_tfp410_platform_data *pdata;
	struct omap_dss_device *dssdev, *in;

	pdata = dev_get_platdata(&pdev->dev);

	ddata->pd_gpio = pdata->power_down_gpio;

	ddata->data_lines = pdata->data_lines;

	in = omap_dss_find_output(pdata->source);
	if (in == NULL) {
		dev_err(&pdev->dev, "Failed to find video source\n");
		return -ENODEV;
	}

	ddata->in = in;

	dssdev = &ddata->dssdev;
	dssdev->name = pdata->name;

	return 0;
}

static int tfp410_probe_of(struct platform_device *pdev)
{
	struct panel_drv_data *ddata = platform_get_drvdata(pdev);
	struct device_node *node = pdev->dev.of_node;
	struct omap_dss_device *in;
	int gpio;

	gpio = of_get_named_gpio(node, "powerdown-gpios", 0);

	if (gpio_is_valid(gpio) || gpio == -ENOENT) {
		ddata->pd_gpio = gpio;
	} else {
		dev_err(&pdev->dev, "failed to parse PD gpio\n");
		return gpio;
	}

	in = omapdss_of_find_source_for_first_ep(node);
	if (IS_ERR(in)) {
		dev_err(&pdev->dev, "failed to find video source\n");
		return PTR_ERR(in);
	}

	ddata->in = in;

	return 0;
}

static int tfp410_probe(struct platform_device *pdev)
{
	struct panel_drv_data *ddata;
	struct omap_dss_device *dssdev;
	int r;

	ddata = devm_kzalloc(&pdev->dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	platform_set_drvdata(pdev, ddata);

	if (dev_get_platdata(&pdev->dev)) {
		r = tfp410_probe_pdata(pdev);
		if (r)
			return r;
	} else if (pdev->dev.of_node) {
		r = tfp410_probe_of(pdev);
		if (r)
			return r;
	} else {
		return -ENODEV;
	}

	if (gpio_is_valid(ddata->pd_gpio)) {
		r = devm_gpio_request_one(&pdev->dev, ddata->pd_gpio,
				GPIOF_OUT_INIT_LOW, "tfp410 PD");
		if (r) {
			dev_err(&pdev->dev, "Failed to request PD GPIO %d\n",
					ddata->pd_gpio);
			goto err_gpio;
		}
	}

	dssdev = &ddata->dssdev;
	dssdev->ops.dvi = &tfp410_dvi_ops;
	dssdev->dev = &pdev->dev;
	dssdev->type = OMAP_DISPLAY_TYPE_DPI;
	dssdev->output_type = OMAP_DISPLAY_TYPE_DVI;
	dssdev->owner = THIS_MODULE;
	dssdev->phy.dpi.data_lines = ddata->data_lines;
	dssdev->port_num = 1;

	r = omapdss_register_output(dssdev);
	if (r) {
		dev_err(&pdev->dev, "Failed to register output\n");
		goto err_reg;
	}

	return 0;
err_reg:
err_gpio:
	omap_dss_put_device(ddata->in);
	return r;
}

static int __exit tfp410_remove(struct platform_device *pdev)
{
	struct panel_drv_data *ddata = platform_get_drvdata(pdev);
	struct omap_dss_device *dssdev = &ddata->dssdev;
	struct omap_dss_device *in = ddata->in;

	omapdss_unregister_output(&ddata->dssdev);

	WARN_ON(omapdss_device_is_enabled(dssdev));
	if (omapdss_device_is_enabled(dssdev))
		tfp410_disable(dssdev);

	WARN_ON(omapdss_device_is_connected(dssdev));
	if (omapdss_device_is_connected(dssdev))
		tfp410_disconnect(dssdev, dssdev->dst);

	omap_dss_put_device(in);

	return 0;
}

static const struct of_device_id tfp410_of_match[] = {
	{ .compatible = "omapdss,ti,tfp410", },
	{},
};

MODULE_DEVICE_TABLE(of, tfp410_of_match);

static struct platform_driver tfp410_driver = {
	.probe	= tfp410_probe,
	.remove	= __exit_p(tfp410_remove),
	.driver	= {
		.name	= "tfp410",
		.of_match_table = tfp410_of_match,
		.suppress_bind_attrs = true,
	},
};

static const struct regmap_config tfp410_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int tfp410_i2c_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct regmap *regmap;
	uint8_t chip_id[4];
	int r;

	/* The regmap is not saved as it is only used by probe */
	regmap = devm_regmap_init_i2c(client, &tfp410_regmap_config);
	if (IS_ERR(regmap)) {
		r = PTR_ERR(regmap);
		dev_err(&client->dev, "Failed to init regmap (%d)\n", r);
		return r;
	}

	/* Check the device ID */
	r = regmap_bulk_read(regmap, TFP410_VEN_ID_L, chip_id, 4);
	if (r < 0) {
		dev_err(&client->dev, "Failed to read device ID (%d)\n", r);
		return r;
	}

	if (!(chip_id[0] == 0x4c && chip_id[1] == 0x01 &&
	      chip_id[2] == 0x10 && chip_id[3] == 0x04)) {
		dev_err(&client->dev, "Unrecognised device "
			"(VEN_ID=0x%02x%02x, DEV_ID=0x%02x%02x)\n",
			chip_id[1], chip_id[0], chip_id[3], chip_id[2]);
		return -ENODEV;
	}

	/* Enable normal operation */
	r = regmap_update_bits(regmap, TFP410_CTL_1_MODE, 0x7f, 0x37);
	if (r < 0) {
		dev_err(&client->dev, "Failed to set CTL_1_MODE (%d)\n", r);
	}

	return r;
}

static const struct i2c_device_id tfp410_i2c_id[] = {
	{ TFP410_I2C_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tfp410_i2c_id);

static struct i2c_driver tfp410_i2c_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.name	= TFP410_I2C_NAME,
		.of_match_table = tfp410_of_match,
	},
	.id_table	= tfp410_i2c_id,
	.probe		= tfp410_i2c_probe,
};

static int __init tfp410_init(void)
{
	int err;

	err = i2c_add_driver(&tfp410_i2c_driver);
	if (err != 0)
		pr_err("tfp410: Failed to register I2C driver (%d)\n", err);

	err = platform_driver_register(&tfp410_driver);
	if (err != 0)
		pr_err("tfp410: Failed to register platform driver (%d)\n",
		       err);

	return 0;
}
module_init(tfp410_init);

static void __exit tfp410_exit(void)
{
	platform_driver_unregister(&tfp410_driver);
	i2c_del_driver(&tfp410_i2c_driver);
}
module_exit(tfp410_exit);

MODULE_AUTHOR("Tomi Valkeinen <tomi.valkeinen@ti.com>");
MODULE_DESCRIPTION("TFP410 DPI to DVI encoder driver");
MODULE_LICENSE("GPL");
