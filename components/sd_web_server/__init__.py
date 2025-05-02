import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import web_server_base
from esphome.const import CONF_PORT

DEPENDENCIES = ['sd_mmc_card']
CODEOWNERS = ['@votre_utilisateur']

sd_web_server_ns = cg.esphome_ns.namespace('sd_web_server')

CONF_WEB_SERVER_ID = 'web_server_id'
CONF_SD_DIR = 'sd_dir'

SDWebServer = sd_web_server_ns.class_(
    'SDWebServer', 
    cg.Component, 
    web_server_base.WebServerBase
)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(SDWebServer),
    cv.Optional(CONF_PORT, default=8080): cv.port,
    cv.Optional(CONF_SD_DIR, default='/sd'): cv.string,
}).extend(cv.COMPONENT_SCHEMA)

def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    yield cg.register_component(var, config)
    
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_sd_directory(config[CONF_SD_DIR]))
    
    yield web_server_base.web_server_base_to_code(var)
