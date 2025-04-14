import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PASSWORD, CONF_USERNAME, CONF_PORT

DEPENDENCIES = ['network']
CODEOWNERS = ['@youkorr']

# Définir les constantes pour la configuration
CONF_ROOT_PATH = 'root_path'

# Créer l'espace de noms et la classe FTP
ftp_ns = cg.esphome_ns.namespace('ftp_server')
FTPServer = ftp_ns.class_('FTPServer', cg.Component)

# Schéma de configuration
CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(FTPServer),
    cv.Required(CONF_USERNAME): cv.string,
    cv.Required(CONF_PASSWORD): cv.string,
    cv.Optional(CONF_ROOT_PATH, default='/sdcard'): cv.string,
    cv.Optional(CONF_PORT, default=21): cv.port,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    
    # Ajouter les paramètres à la classe C++
    cg.add(var.set_username(config[CONF_USERNAME]))
    cg.add(var.set_password(config[CONF_PASSWORD]))
    cg.add(var.set_root_path(config[CONF_ROOT_PATH]))
    cg.add(var.set_port(config[CONF_PORT]))










