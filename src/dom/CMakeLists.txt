
set(dom_SRC
	cssreader.cpp
	domimpl.cpp
	domptr.cpp
	domstring.cpp
	lsimpl.cpp
	prop-css2.cpp
	prop-css.cpp
	prop-svg.cpp
	smilimpl.cpp
	svgimpl.cpp
	svgreader.cpp
	ucd.cpp
	uri.cpp
	xmlreader.cpp
	xpathimpl.cpp
	xpathparser.cpp
	xpathtoken.cpp
	
	io/domstream.cpp
	
	util/ziptool.cpp


	# -------
	# Headers
	css.h
	cssreader.h
	dom.h
	domimpl.h
	domptr.h
	domstring.h
	events.h
	ls.h
	lsimpl.h
	smil.h
	smilimpl.h
	stylesheets.h
	svg.h
	svgimpl.h
	svgreader.h
	svgtypes.h
	traversal.h
	ucd.h
	uri.h
	views-level3.h
	views.h
	xmlreader.h
	xmlwriter.h
	xpath.h
	xpathimpl.h
	xpathparser.h
	xpathtoken.h

	io/domstream.h

	util/ziptool.h
)

add_inkscape_lib(dom_LIB "${dom_SRC}")
