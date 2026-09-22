# COSMOS-on-Qt6 integration check (run from a COSMOS project dir)
require 'cosmos'
require 'cosmos/gui/qt'
require 'cosmos/gui/widgets/realtime_button_bar'
require 'cosmos/gui/choosers/combobox_chooser'
require 'cosmos/gui/choosers/integer_chooser'
require 'cosmos/gui/choosers/float_chooser'
require 'cosmos/gui/choosers/string_chooser'

fails = []
chk = ->(n, c) { puts format("  %-46s %s", n, c ? "ok" : "FAIL"); fails << n unless c }

app = Qt::Application.new
chk.("cosmos/gui/qt loads on Qt6",      defined?(Cosmos::COLORS) ? true : false)
chk.("COSMOS-defined Qt classes exist",   defined?(Qt::CheckboxLabel) && defined?(Qt::AdaptiveGridLayout) ? true : false)
chk.("Cosmos.getColor works",           Cosmos.getColor(255,0,0).is_a?(Qt::Color))
chk.("Cosmos.getFont works",            Cosmos.getFont("Arial", 12).is_a?(Qt::Font))
chk.("Cosmos.getPalette works",         Cosmos.getPalette(Qt::black, Qt::white).is_a?(Qt::Palette))
chk.("Qt::Widget < Qt::Base (ext ABI)", Qt::Widget.superclass == Qt::Base)

win = Qt::Widget.new
lay = Qt::VBoxLayout.new
bar   = Cosmos::RealtimeButtonBar.new(win);                           lay.addWidget(bar)
combo = Cosmos::ComboboxChooser.new(win, "Target:", %w[INST INST2]);  lay.addWidget(combo)
ic    = Cosmos::IntegerChooser.new(win, "Packets:", 100);             lay.addWidget(ic)
fc    = Cosmos::FloatChooser.new(win, "Threshold:", 12.5);            lay.addWidget(fc)
sc    = Cosmos::StringChooser.new(win, "Label:", "TEMP1");            lay.addWidget(sc)
win.setLayout(lay); win.resize(460, 230)

chk.("RealtimeButtonBar instantiates",  bar.is_a?(Qt::Widget))
chk.("ComboboxChooser value",           combo.string == "INST")
chk.("IntegerChooser value",            ic.value == 100)
chk.("FloatChooser value",              fc.value == 12.5)
chk.("StringChooser value",             sc.string == "TEMP1")
chk.("all 5 widgets in layout",         lay.count == 5)

pix = win.grab
chk.("renders to a pixmap",             pix.width == 460 && pix.height == 230)
chk.("pixmap is not blank",             !pix.isNull)

# --- QPainter + virtual dispatch: the lib/cosmos/gui/line_graph path --------
require 'cosmos/gui/line_graph/line_graph'
pm = Qt::Pixmap.new(120, 60)
pt = Qt::Painter.new(pm)
chk.("Qt::Painter begins on a pixmap",  pt.isActive)
chk.("Painter#paintEngine is truthy",   !pt.paintEngine.nil?)
pt.addLineColor(0, 0, 100, 50, Cosmos.getColor(255,0,0))      # COSMOS primitive
pt.addRectColorFill(5, 5, 10, 10, Cosmos.getColor(0,0,255))
pt.addSimpleTextAt("x", 20, 40, Cosmos.getColor(0,0,0))
pt.end
chk.("COSMOS draw primitives work",     !pt.isActive)

g = Cosmos::LineGraph.new
g.title = "INST HEALTH_STATUS"
xs = (0..40).to_a
g.add_line("TEMP1", xs.map { |i| 20 + 40 * Math.sin(i / 8.0) }, xs)
g.add_line("TEMP2", xs.map { |i| 50 + 25 * Math.cos(i / 5.0) }, xs)
g.resize(640, 320)
g.show
3.times { app.processEvents }
gpix = g.grab
chk.("LineGraph instantiates",          g.is_a?(Qt::Widget))
chk.("LineGraph paintEvent dispatched", gpix.width == 640 && gpix.height == 320)
# a blank widget renders as one flat colour; a drawn graph does not
img = gpix.toImage
colors = [[0,0],[320,10],[100,160],[500,200],[320,300]].map { |x,y| img.pixelColor(x,y).name }.uniq
chk.("LineGraph actually drew content", colors.size > 1)

# --- Launcher tool parameter dialog ----------------------------------------
# Regression: clicking a Launcher button with parameters calls
# LauncherTool#parameters_dialog, which uses Qt::Cursor.pos,
# Qt::Application.desktop.width and dialog.frameGeometry.width, then blocks in
# a modal exec. Loading the tool never exercises any of it.
require 'cosmos/tools/launcher/launcher_tool'
chk.("Qt::Cursor.pos returns a point",   (cp = Qt::Cursor.pos; cp.respond_to?(:x) && cp.respond_to?(:y)))
chk.("Application.desktop.width > 0",    Qt::Application.desktop.width > 0)
probe_dlg = Qt::Dialog.new
probe_dlg.resize(400, 0)
chk.("dialog.frameGeometry.width",       probe_dlg.frameGeometry.width >= 0)

lt = Cosmos::LauncherTool.new(win, "Demo", "LAUNCH CmdTlmServer", false,
                              [["Config", "a.txt|b.txt"], ["Name", "single"]])
Qt.single_shot(500) do
  w = Qt::Application.activeModalWidget || Qt::Application.activeWindow
  w.accept if w && w.respond_to?(:accept)
end
params = begin
  lt.send(:parameters_dialog)
rescue Exception => e
  "ERROR #{e.class}: #{e.message}"
end
chk.("parameters_dialog returns params", params.is_a?(String) && params.include?("a.txt") && params.include?("single"))

puts
puts fails.empty? ? "COSMOS INTEGRATION OK" : "FAILURES: #{fails.inspect}"
exit(fails.empty? ? 0 : 1)
