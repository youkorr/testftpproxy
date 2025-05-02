import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PORT

CODEOWNERS = ["@ton_pseudo"]
DEPENDENCIES = ["sd_mmc_card"]

sd_web_server_ns = cg.esphome_ns.namespace("sd_web_server")
SDWebServer = sd_web_server_ns.class_("SDWebServer", cg.Component)

CONF_SD_DIR = "sd_dir"

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(SDWebServer),
    cv.Optional(CONF_PORT, default=8080): cv.port,
    cv.Optional(CONF_SD_DIR, default="/sdcard"): cv.string,
}).extend(cv.COMPONENT_SCHEMA)

def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    yield cg.register_component(var, config)
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_sd_directory(config[CONF_SD_DIR]))


