package main

//
// #include <stdint.h>
// typedef struct {
//   uint32_t annotation_points;
//   char **annotations;
//   char *formatted;
// } FormatAnnotateResult;
import "C"
import (
	"fmt"
	"go/format"
	"go/scanner"
	"go/token"
	"io"
	"os"
)

type Annotation struct {
	index      uint32
	annotation string
}

//export FormatAnnotate
func FormatAnnotate(toformat *C.char) *C.FormatAnnotateResult {
	result := (*C.FormatAnnotateResult)(C.malloc(C.sizeof_FormatAnnotateResult))
	result.annotation_points = C.uint32_t(5)
	return result
}
func doFormat(toFormat *string) (*string, error) {
	formatted, err := format.Source([]byte(*toFormat))
	if err != nil {
		return nil, err
	}
	formatted_str := string(formatted)
	return &formatted_str, nil
}

func doAnnotate(toAnnotate *string) (*map[uint32]*Annotation, error) {
	// src is the input that we want to tokenize.
	annotations := make(map[uint32]*Annotation, 0)
	src := []byte(*toAnnotate)

	// Initialize the scanner.
	var s scanner.Scanner
	fset := token.NewFileSet()                      // positions are relative to fset
	file := fset.AddFile("", fset.Base(), len(src)) // register input "file"
	s.Init(file, src, nil /* no error handler */, scanner.ScanComments)

	// Repeated calls to Scan yield the token sequence found in the input.
	for {
		pos, tok, literal := s.Scan()
		if tok == token.EOF {
			break
		}
		annotationStart := uint32(fset.Position(pos).Offset)
		annotationEnd := annotationStart + uint32(fset.Position(pos).Offset+len(literal))

		fmt.Printf("annotationStart: %v\n", annotationStart)
		fmt.Printf("annotationEnd: %v\n", annotationEnd)

		if tok.IsKeyword() {
			startAnnotation := Annotation{annotationStart, "<font color=green>"}
			endAnnotation := Annotation{annotationEnd, "</font>"}
			annotations[annotationStart] = &startAnnotation
			annotations[annotationEnd] = &endAnnotation
		} else if tok.IsLiteral() {
			startAnnotation := Annotation{annotationStart, "<font color=red>"}
			endAnnotation := Annotation{annotationEnd, "</font>"}
			annotations[annotationStart] = &startAnnotation
			annotations[annotationEnd] = &endAnnotation
		} else if tok.IsOperator() {
			startAnnotation := Annotation{annotationStart, "<font color=blue>"}
			endAnnotation := Annotation{annotationEnd, "</font>"}
			annotations[annotationStart] = &startAnnotation
			annotations[annotationEnd] = &endAnnotation
		} else if tok == token.COMMENT {
			startAnnotation := Annotation{annotationStart, "<font color=gray>"}
			endAnnotation := Annotation{annotationEnd, "</font>"}
			annotations[annotationStart] = &startAnnotation
			annotations[annotationEnd] = &endAnnotation
		}
	}
	return &annotations, nil
}

func doFormatAnnotate(toFormat *string) (*string, *map[uint32]*Annotation, error) {
	formatted, err := doFormat(toFormat)
	if err != nil {
		return nil, nil, err
	}
	annotations, err := doAnnotate(formatted)
	return formatted, annotations, nil
}

func printAnnotated(formatted string, annotations *map[uint32]*Annotation) {
	for i := uint32(0); i < uint32(len(formatted)); i++ {
		if (*annotations)[i] != nil {
			fmt.Printf("i: %v\n", i)
			fmt.Print((*annotations)[i].annotation)
		}
		fmt.Print(string(formatted[i]))
	}
}

func testFormatAnnotate(filename string) error {
	file, err := os.OpenFile(filename, os.O_RDONLY, os.ModeAppend)
	if err != nil {
		return err
	}

	contents, err := io.ReadAll(file)
	if err != nil {
		return err
	}

	str_contents := string(contents)
	formatted, annotations, err := doFormatAnnotate(&str_contents)

	if err != nil {
		return err
	}

	fmt.Printf("formatted:\n%v\n", *formatted)
	fmt.Printf("annotations:\n%v\n", *annotations)

	printAnnotated(*formatted, annotations)

	return nil
}

func main() {
	if err := testFormatAnnotate("testing.go"); err != nil {
		fmt.Printf("err: %v\n", err)
	} else {
		fmt.Printf("success!\n")
	}
}
