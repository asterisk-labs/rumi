.onLoad <- function(libname, pkgname) {
  reg.finalizer(asNamespace(pkgname), function(e) {
    if ("rumi" %in% names(getLoadedDLLs()))
      .Call(rumi_r_shutdown_pool)
  }, onexit = TRUE)
}

.onUnload <- function(libpath) {
  .Call(rumi_r_shutdown_pool)
}
