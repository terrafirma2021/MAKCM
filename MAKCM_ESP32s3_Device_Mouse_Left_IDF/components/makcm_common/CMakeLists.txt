# Reuse the protocol/state-machine sources shared with the Arduino target.
# There is intentionally no compiled source in this component; the public
# include path points at the repository-level, platform-independent headers.
idf_component_register(INCLUDE_DIRS "../../../common")
