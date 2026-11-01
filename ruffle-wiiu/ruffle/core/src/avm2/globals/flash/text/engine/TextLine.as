package flash.text.engine {
    import flash.display.DisplayObject;
    import flash.display.DisplayObjectContainer;
    import flash.errors.IllegalOperationError;
    import flash.geom.Point;
    import flash.geom.Rectangle;
    import flash.ui.ContextMenu;

    // FIXME: None of the DisplayObjectContainer methods actually work on
    // the TextLine class in Ruffle, despite the methods working fine in FP-
    // however, it's unlikely that SWFs will actually attempt to add children
    // to a TextLine.
    [Ruffle(Abstract)]
    [API("662")]
    public final class TextLine extends DisplayObjectContainer {
        public static const MAX_LINE_WIDTH:int = 1000000;

        public var userData;

        public native function get rawTextLength():int;

        public native function get textBlockBeginIndex():int;

        public native function get specifiedWidth():Number;

        public native function get textBlock():TextBlock;

        public native function get ascent():Number;

        [API("670")]
        public function get totalAscent():Number {
            return this.ascent;
        }

        public native function get descent():Number;

        [API("670")]
        public function get totalDescent():Number {
            return this.descent;
        }

        public function get unjustifiedTextWidth():Number {
            return this.textWidth;
        }

        public native function get textWidth():Number;
        public native function get textHeight():Number;

        public native function get validity():String;
        public native function set validity(value:String):void;

        public native function get hasGraphicElement():Boolean;

        // A surrogate pair is one atom; any other code unit is one too. Gives the unit each atom starts at, then the end.
        private function atomStarts():Array {
            var starts:Array = [];
            var text:String = this.textBlock && this.textBlock.content ? this.textBlock.content.rawText : null;
            var begin:int = this.textBlockBeginIndex;
            var end:int = begin + this.rawTextLength;
            for (var i:int = begin; i < end; i++) {
                starts.push(i);
                if (text != null && i + 1 < end) {
                    var unit:Number = text.charCodeAt(i);
                    if (unit >= 0xD800 && unit <= 0xDBFF) {
                        i++;
                    }
                }
            }
            starts.push(end);
            return starts;
        }

        private function checkedAtom(index:int):Array {
            var starts:Array = this.atomStarts();
            if (index < 0 || index >= starts.length - 1) {
                Error.throwError(RangeError, 2006);
            }
            return starts;
        }

        public function get atomCount():int {
            return this.atomStarts().length - 1;
        }

        public native function get previousLine():TextLine;
        public native function get nextLine():TextLine;

        public function getBaselinePosition(baseline:String):Number {
            switch (baseline) {
                case TextBaseline.ROMAN:
                    return 0.0;
                case TextBaseline.ASCENT:
                case TextBaseline.IDEOGRAPHIC_TOP:
                    return -this.ascent;
                case TextBaseline.DESCENT:
                case TextBaseline.IDEOGRAPHIC_BOTTOM:
                    return this.descent;
                case TextBaseline.IDEOGRAPHIC_CENTER:
                    return (this.descent - this.ascent) / 2;
            }
            Error.throwError(ArgumentError, 2004);
            return 0.0;
        }

        public native function get hasTabs():Boolean;

        private native function charLeft(index:int):Number;
        private native function charRight(index:int):Number;
        private native function graphicAt(index:int):DisplayObject;

        public function getAtomIndexAtPoint(stageX:Number, stageY:Number):int {
            var local:Point = this.globalToLocal(new Point(stageX, stageY));
            if (local.y < -this.ascent || local.y > this.descent) {
                return -1;
            }
            var starts:Array = this.atomStarts();
            var begin:int = this.textBlockBeginIndex;
            for (var i:int = 0; i < starts.length - 1; i++) {
                if (local.x >= this.charLeft(starts[i] - begin) && local.x < this.charRight(starts[i + 1] - 1 - begin)) {
                    return i;
                }
            }
            return -1;
        }

        public function getAtomIndexAtCharIndex(charIndex:int):int {
            var starts:Array = this.atomStarts();
            for (var i:int = 0; i < starts.length - 1; i++) {
                if (charIndex >= starts[i] && charIndex < starts[i + 1]) {
                    return i;
                }
            }
            return -1;
        }

        public function getAtomBidiLevel(index:int):int {
            this.checkedAtom(index);
            return this.textBlock ? this.textBlock.bidiLevel : 0;
        }

        public function getAtomBounds(index:int):Rectangle {
            var starts:Array = this.checkedAtom(index);
            var begin:int = this.textBlockBeginIndex;
            var left:Number = this.charLeft(starts[index] - begin);
            var right:Number = this.charRight(starts[index + 1] - 1 - begin);
            return new Rectangle(left, -this.ascent, right - left, this.ascent + this.descent);
        }

        public function getAtomCenter(index:int):Number {
            var bounds:Rectangle = this.getAtomBounds(index);
            return bounds.x + bounds.width / 2;
        }

        public function getAtomGraphic(index:int):DisplayObject {
            var starts:Array = this.checkedAtom(index);
            return this.graphicAt(starts[index] - this.textBlockBeginIndex);
        }

        public function getAtomTextBlockBeginIndex(index:int):int {
            return this.checkedAtom(index)[index];
        }

        public function getAtomTextBlockEndIndex(index:int):int {
            return this.checkedAtom(index)[index + 1];
        }

        public function getAtomTextRotation(index:int):String {
            this.checkedAtom(index);
            return TextRotation.ROTATE_0;
        }

        public function getAtomWordBoundaryOnLeft(index:int):Boolean {
            var starts:Array = this.checkedAtom(index);
            if (index == 0) {
                return true;
            }
            var text:String = this.textBlock && this.textBlock.content ? this.textBlock.content.rawText : null;
            if (text == null) {
                return false;
            }
            return text.charAt(starts[index - 1]) == " " && text.charAt(starts[index]) != " ";
        }

        // This function does nothing in Flash Player 32
        public function flushAtomData():void { }

        // Overrides

        override public function set contextMenu(cm:ContextMenu):void {
            Error.throwError(IllegalOperationError, 2181);
        }

        override public function set focusRect(value:Object):void {
            Error.throwError(IllegalOperationError, 2181);
        }

        override public function set tabChildren(value:Boolean):void {
            Error.throwError(IllegalOperationError, 2181);
        }

        override public function set tabEnabled(value:Boolean):void {
            Error.throwError(IllegalOperationError, 2181);
        }

        override public function set tabIndex(index:int):void {
            Error.throwError(IllegalOperationError, 2181);
        }

        // End of overrides
    }
}
